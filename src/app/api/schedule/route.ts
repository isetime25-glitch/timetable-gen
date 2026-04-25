import { NextRequest, NextResponse } from 'next/server';
import { createClient } from '@/lib/supabase/server';
import { runScheduler, ScheduleInput, slotIndexToTime } from '@/lib/engine/solver-bridge';

// Local slot type — avoids dependency on exported ScheduledSlot
interface SolverSlot {
  teacher_id: string;
  subject_id: string;
  room_id: string;
  day_of_week: number;
  slot_index: number;
  slot_status: string;
  is_lab: boolean;
  is_double_start: boolean;
  metadata: { conflict_reason: string };
}

export const dynamic = 'force-dynamic';

// ============================================================
// POST /api/schedule
// ============================================================
export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const { department_id, semester, section = 'A' } = body;

    if (!department_id) {
      return NextResponse.json({ error: 'department_id is required' }, { status: 400 });
    }

    const supabase = await createClient();

    const [teachersRes, subjectsRes, roomsRes, assignmentsRes] = await Promise.all([
      supabase
        .from('teachers')
        .select('id, name, is_available, max_daily_slots')
        .eq('department_id', department_id),

      // Fetch L, T, P hours separately so we can compute actual weekly slot count.
      // VTU rule: each lecture hour = 1 slot, each tutorial hour = 1 slot,
      // each practical hour = 1 slot (2 practical hours = 1 lab pair = 2 slots).
      // weekly_credits (slots) = lecture_hours + tutorial_hours + practical_hours
      supabase
        .from('subjects')
        .select('id, code, lecture_hours, tutorial_hours, practical_hours, preferred_room_type')
        .eq('department_id', department_id)
        .eq('semester', semester ?? 1),

      supabase
        .from('room_pool')
        .select('id, room_name, capacity, room_type')
        .eq('department_id', department_id),

      supabase
        .from('teacher_subject_assignments')
        .select('teacher_id, subject_id')
        .eq('department_id', department_id),
    ]);

    for (const res of [teachersRes, subjectsRes, roomsRes, assignmentsRes]) {
      if (res.error) {
        return NextResponse.json(
          { error: `Supabase error: ${res.error.message}` },
          { status: 500 }
        );
      }
    }

    const teachers = teachersRes.data ?? [];
    const subjects = subjectsRes.data ?? [];
    const rooms = roomsRes.data ?? [];
    const assignments = assignmentsRes.data ?? [];

    if (teachers.length === 0)
      return NextResponse.json({ error: 'No teachers found for this department' }, { status: 400 });
    if (subjects.length === 0)
      return NextResponse.json({ error: 'No subjects found for this department and semester' }, { status: 400 });
    if (assignments.length === 0)
      return NextResponse.json({ error: 'No teacher-subject assignments found.' }, { status: 400 });

    const solverInput: ScheduleInput = {
      department_id,
      teachers: teachers.map((t) => ({
        id: t.id,
        name: t.name,
        is_available: t.is_available ?? true,
        max_daily_slots: t.max_daily_slots ?? 4,
      })),
      subjects: subjects.map((s) => {
        const lectureHours = (s as any).lecture_hours ?? 0;
        const tutorialHours = (s as any).tutorial_hours ?? 0;
        const practicalHours = (s as any).practical_hours ?? 0;
        // Total weekly slots = all contact hours per week
        // practical_hours must be even (VTU guarantees this: P is always 0 or 2)
        const weeklySlots = lectureHours + tutorialHours + practicalHours;

        return {
          id: s.id,
          code: s.code,
          weekly_credits: weeklySlots,
          practical_hours: practicalHours,
          preferred_room_type: (s.preferred_room_type ?? 'lecture')
            .toLowerCase()
            .replace(/\s+/g, '_'),
        };
      }),
      rooms: rooms.map((r) => ({
        id: r.id,
        room_name: r.room_name,
        capacity: r.capacity ?? 60,
        room_type: (r.room_type ?? 'lecture')
          .toLowerCase()
          .replace(/\s+/g, '_'),
      })),
      assignments,
      days_per_week: 5,
      slots_per_day: 6,
      lunch_slot_index: 3,
      time_limit_seconds: 60,
    };

    // ── Compute blocked_slots from other sections' timetables ──
    // Fetch all slots for this department that belong to OTHER sections
    const { data: otherSectionSlots } = await supabase
      .from('timetable_slots')
      .select('teacher_id, day_of_week, start_time')
      .eq('department_id', department_id)
      .neq('section', section);

    if (otherSectionSlots && otherSectionSlots.length > 0) {
      // Convert start_time to slot_index (09:00 = 0, 10:00 = 1, etc.)
      const timeToSlot = (t: string) => {
        const [h, m] = t.split(':').map(Number);
        return Math.floor((h * 60 + m - 9 * 60) / 60);
      };

      solverInput.blocked_slots = otherSectionSlots.map((s) => ({
        teacher_id: s.teacher_id,
        day: s.day_of_week - 1,   // solver uses 0-indexed days
        slot: timeToSlot(s.start_time),
      }));
    }

    const result = await runScheduler(solverInput);

    if (!result.success) {
      return NextResponse.json(
        {
          error: 'Solver failed',
          status: result.status,
          details: result.error,
          conflict_log: result.conflict_log,
        },
        { status: 500 }
      );
    }

    // Clear existing unlocked slots for this department
    const { error: deleteError } = await supabase
      .from('timetable_slots')
      .delete()
      .eq('department_id', department_id)
      .eq('section', section)
      .eq('is_locked', false);

    if (deleteError) {
      return NextResponse.json(
        { error: `Failed to clear old slots: ${deleteError.message}` },
        { status: 500 }
      );
    }



    const slotRows = (result.slots as SolverSlot[]).map((slot) => {
      const { start_time, end_time } = slotIndexToTime(slot.slot_index);
      return {
        department_id,
        section,
        teacher_id: slot.teacher_id,
        subject_id: slot.subject_id,
        room_id: slot.room_id || null,
        day_of_week: slot.day_of_week,
        start_time,
        end_time,
        slot_status: slot.slot_status,
        is_locked: false,
        is_lab: slot.is_lab,
        is_double_start: slot.is_double_start,
        metadata: slot.metadata,
      };
    });

    const { error: insertError } = await supabase
      .from('timetable_slots')
      .insert(slotRows);

    if (insertError) {
      return NextResponse.json(
        { error: `Failed to save slots: ${insertError.message}` },
        { status: 500 }
      );
    }

    await supabase.from('audit_logs').insert({
      action_type: 'AUTO_GENERATE',
      table_name: 'timetable_slots',
      record_id: crypto.randomUUID(),
      new_data: {
        department_id,
        semester,
        section,
        slots_generated: result.slots.length,
        solver_status: result.status,
      },
    });

    return NextResponse.json({
      success: true,
      status: result.status,
      slots_generated: result.slots.length,
      conflict_log: result.conflict_log,
    });

  } catch (error) {
    console.error('[/api/schedule POST]', error);
    return NextResponse.json({ error: 'Internal server error' }, { status: 500 });
  }
}

// ============================================================
// GET /api/schedule?department_id=xxx
// ============================================================
export async function GET(request: NextRequest) {
  try {
    const { searchParams } = new URL(request.url);
    const department_id = searchParams.get('department_id');
    const section = searchParams.get('section') || 'A';
    const semester = searchParams.get('semester');

    if (!department_id) {
      return NextResponse.json(
        { error: 'department_id query param is required' },
        { status: 400 }
      );
    }

    const supabase = await createClient();

    let query = supabase
      .from('timetable_slots')
      .select(`
        id,
        day_of_week,
        start_time,
        end_time,
        slot_status,
        is_locked,
        is_lab,
        is_double_start,
        metadata,
        created_at,
        section,
        teachers  ( id, name, email ),
        subjects  ( id, code, title, weekly_credits, preferred_room_type, semester ),
        room_pool ( id, room_name, room_type, capacity )
      `)
      .eq('department_id', department_id)
      .eq('section', section)
      .order('day_of_week', { ascending: true })
      .order('start_time', { ascending: true });

    if (semester) {
      query = query.eq('subjects.semester', parseInt(semester, 10));
    }

    const { data, error } = await query;

    if (error) {
      return NextResponse.json({ error: error.message }, { status: 500 });
    }

    // When filtering by semester via the join, rows whose subject doesn't match
    // will have subjects set to null. Filter those out.
    const filtered = semester
      ? (data ?? []).filter((row: any) => row.subjects !== null)
      : (data ?? []);

    return NextResponse.json({ slots: filtered });

  } catch (error) {
    console.error('[/api/schedule GET]', error);
    return NextResponse.json({ error: 'Internal server error' }, { status: 500 });
  }
}

// ============================================================
// DELETE /api/schedule?department_id=xxx
// ============================================================
export async function DELETE(request: NextRequest) {
  try {
    const { searchParams } = new URL(request.url);
    const department_id = searchParams.get('department_id');
    const section = searchParams.get('section') || 'A';

    if (!department_id) {
      return NextResponse.json({ error: 'department_id is required' }, { status: 400 });
    }

    const supabase = await createClient();

    const { error } = await supabase
      .from('timetable_slots')
      .delete()
      .eq('department_id', department_id)
      .eq('section', section)
      .eq('is_locked', false);

    if (error) {
      return NextResponse.json({ error: error.message }, { status: 500 });
    }

    await supabase.from('audit_logs').insert({
      action_type: 'CLEAR_TIMETABLE',
      table_name: 'timetable_slots',
      record_id: crypto.randomUUID(),
      new_data: { department_id },
    });

    return NextResponse.json({ success: true });

  } catch (error) {
    console.error('[/api/schedule DELETE]', error);
    return NextResponse.json({ error: 'Internal server error' }, { status: 500 });
  }
}