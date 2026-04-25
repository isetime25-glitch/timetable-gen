"use client";

import { useState, useEffect, useCallback, useRef } from "react";
import TimetableEditor from "@/components/features/timetable-editor";
import html2canvas from "html2canvas-pro";
import { jsPDF } from "jspdf";

interface Props {
  departmentId: string;
}

interface RawSlot {
  id: string;
  day_of_week: number;
  start_time: string;
  end_time: string;
  slot_status: string;
  is_locked: boolean;
  is_lab: boolean;
  is_double_start: boolean;
  metadata: { conflict_reason?: string } | null;
  teachers: { id: string; name: string; email: string } | null;
  subjects: { id: string; code: string; title: string; weekly_credits: number; preferred_room_type: string } | null;
  room_pool: { id: string; room_name: string; room_type: string; capacity: number } | null;
}

function timeToSlotIndex(startTime: string): number {
  const [h, m] = startTime.split(":").map(Number);
  return Math.floor((h * 60 + m - 9 * 60) / 60);
}

interface Collision {
  slot_id: string;
  day_of_week: number;
  start_time: string;
  teacher_name: string;
  other_department: string;
}

interface SlotGridData {
  code: string;
  isLab: boolean;
  isDoubleStart: boolean;
  isConflict: boolean;
  conflictInfo?: string;
  isLocked: boolean;
  slotId: string;
  subjectId?: string;
}

function buildGrid(slots: RawSlot[], collisions: Collision[] = []) {
  // Build collision lookup: "day-slotIdx" → collision info
  const collisionMap = new Map<string, Collision>();
  collisions.forEach(c => {
    const slotIdx = timeToSlotIndex(c.start_time);
    collisionMap.set(`${c.day_of_week - 1}-${slotIdx}`, c);
  });

  const grid: (SlotGridData | null)[][] = Array.from(
    { length: 5 },
    () => Array(6).fill(null)
  );
  slots.forEach((slot) => {
    const dayIdx = slot.day_of_week - 1;
    const slotIdx = timeToSlotIndex(slot.start_time);
    if (dayIdx < 0 || dayIdx > 4) return;
    if (slotIdx < 0 || slotIdx > 5) return;
    const collision = collisionMap.get(`${dayIdx}-${slotIdx}`);
    grid[dayIdx][slotIdx] = {
      ...toSlotData(slot),
      isConflict: !!collision,
      conflictInfo: collision ? `⚠ ${collision.teacher_name} also at ${collision.other_department}` : undefined,
    };
  });
  return grid;
}

function toSlotData(slot: RawSlot) {
  return {
    code: slot.subjects?.code ?? "—",
    isLab: slot.is_lab ?? false,
    isDoubleStart: slot.is_double_start ?? false,
    isConflict: !!slot.metadata?.conflict_reason,
    isLocked: slot.is_locked,
    slotId: slot.id,
    subjectId: slot.subjects?.id,
  };
}

function buildLegend(slots: RawSlot[]) {
  const seen = new Set<string>();
  const legend: { code: string; title: string; initials: string; faculty: string }[] = [];
  slots.forEach((slot) => {
    if (!slot.subjects || seen.has(slot.subjects.code)) return;
    seen.add(slot.subjects.code);
    legend.push({
      code: slot.subjects.code,
      title: slot.subjects.title,
      initials: slot.teachers?.name?.split(" ").map((w: string) => w[0]).join("") ?? "—",
      faculty: slot.teachers?.name ?? "—",
    });
  });
  return legend;
}

export default function TimetableClient({ departmentId }: Props) {
  const [slots, setSlots] = useState<RawSlot[]>([]);
  const [collisions, setCollisions] = useState<Collision[]>([]);
  const [loading, setLoading] = useState(false);
  const [generating, setGenerating] = useState(false);
  const [clearing, setClearing] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<string | null>(null);
  const [gridKey, setGridKey] = useState(0);

  // ── Editable header fields ──
  const [institutionName, setInstitutionName] = useState("MAHARAJA INSTITUTE OF TECHNOLOGY MYSORE");
  const [departmentName, setDepartmentName] = useState("Department of Information Science & Engineering");
  const [tableTitle, setTableTitle] = useState("TIME TABLE - 2024-25");
  const [semesterInfo, setSemesterInfo] = useState("I Semester Section – D (IS)");
  const [roomNo, setRoomNo] = useState("");
  const [downloadingPdf, setDownloadingPdf] = useState(false);
  const gridRef = useRef<HTMLDivElement>(null);
  const [editingHeader, setEditingHeader] = useState(false);
  const [selectedSemester, setSelectedSemester] = useState<number>(1);
  const [selectedSection, setSelectedSection] = useState<string>('A');

  const fetchTimetable = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const [schedRes, collRes] = await Promise.all([
        fetch(`/api/schedule?department_id=${departmentId}&section=${selectedSection}&semester=${selectedSemester}&t=${Date.now()}`, {
          cache: "no-store",
          headers: { "Cache-Control": "no-cache" },
        }),
        fetch(`/api/collisions?department_id=${departmentId}&t=${Date.now()}`, {
          cache: "no-store",
        }),
      ]);
      const data = await schedRes.json();
      if (!schedRes.ok) throw new Error(data.error ?? "Failed to fetch timetable");
      setSlots([...(data.slots ?? [])]);

      const collData = await collRes.json();
      setCollisions(collData.collisions ?? []);

      setGridKey(k => k + 1);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setLoading(false);
    }
  }, [departmentId, selectedSection, selectedSemester]);

  useEffect(() => { fetchTimetable(); }, [fetchTimetable]);

  async function generateTimetable() {
    setGenerating(true);
    setError(null);
    setStatus(null);
    try {
      const res = await fetch("/api/schedule", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ department_id: departmentId, semester: selectedSemester, section: selectedSection }),
      });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error ?? "Solver failed");
      setStatus(`✓ ${data.status} — ${data.slots_generated} slots generated`);
      await fetchTimetable();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setGenerating(false);
    }
  }

  async function clearTimetable() {
    if (!confirm("Clear all unlocked slots? This cannot be undone.")) return;
    setClearing(true);
    setError(null);
    setStatus(null);
    try {
      const res = await fetch(`/api/schedule?department_id=${departmentId}&section=${selectedSection}`, { method: "DELETE" });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error ?? "Failed to clear");
      setStatus("✓ Timetable cleared");
      await fetchTimetable();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setClearing(false);
    }
  }

  const grid = buildGrid(slots, collisions);
  const legend = buildLegend(slots);

  const inputStyle = {
    border: "1px solid #C8C0A8",
    borderRadius: 3,
    padding: "3px 6px",
    fontSize: 12,
    fontFamily: "'Georgia', serif",
    background: "#FFFDF5",
    color: "#2D3436",
    width: "100%",
  };

  return (
    <div style={{ minHeight: "100vh", background: "#F0EBE0", padding: "32px 16px", fontFamily: "'Georgia', serif" }}>

      {/* ── Toolbar ── */}
      <div style={{ maxWidth: 1100, margin: "0 auto 16px", display: "flex", alignItems: "center", justifyContent: "space-between", gap: 12 }}>
        <h1 style={{ fontSize: 18, fontWeight: 700, color: "#2D3436", margin: 0 }}>Timetable Generator</h1>
        <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
          {status && <span style={{ fontSize: 12, color: "#27AE60", fontFamily: "monospace" }}>{status}</span>}
          {error && <span style={{ fontSize: 12, color: "#C0392B", fontFamily: "monospace" }}>✗ {error}</span>}

          <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <label style={{ fontSize: 11, fontWeight: 700, color: "#8B7D6B" }}>SEM</label>
            <select
              value={selectedSemester}
              onChange={e => setSelectedSemester(Number(e.target.value))}
              style={{ padding: "7px 10px", border: "1.5px solid #2D3436", borderRadius: 4, fontSize: 12, background: "#FFFDF5", color: "#2D3436", cursor: "pointer" }}
            >
              {[1, 2, 3, 4, 5, 6, 7, 8].map(s => (
                <option key={s} value={s}>Semester {s}</option>
              ))}
            </select>
          </div>

          <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <label style={{ fontSize: 11, fontWeight: 700, color: "#8B7D6B" }}>SEC</label>
            <select
              value={selectedSection}
              onChange={e => setSelectedSection(e.target.value)}
              style={{ padding: "7px 10px", border: "1.5px solid #2D3436", borderRadius: 4, fontSize: 12, background: "#FFFDF5", color: "#2D3436", cursor: "pointer" }}
            >
              {['A', 'B', 'C', 'D'].map(s => (
                <option key={s} value={s}>Section {s}</option>
              ))}
            </select>
          </div>

          <button onClick={() => setEditingHeader(e => !e)} style={{ padding: "8px 14px", background: editingHeader ? "#2D3436" : "transparent", border: "1.5px solid #2D3436", borderRadius: 4, fontSize: 12, fontWeight: 600, color: editingHeader ? "#FFFDF5" : "#2D3436", cursor: "pointer" }}>
            {editingHeader ? "✓ Done" : "✎ Edit Header"}
          </button>

          <button onClick={fetchTimetable} disabled={loading} style={{ padding: "8px 14px", background: "transparent", border: "1.5px solid #2D3436", borderRadius: 4, fontSize: 12, fontWeight: 600, color: "#2D3436", cursor: loading ? "not-allowed" : "pointer" }}>
            {loading ? "Loading…" : "↻ Refresh"}
          </button>

          <button onClick={clearTimetable} disabled={clearing} style={{ padding: "8px 14px", background: "transparent", border: "1.5px solid #C0392B", borderRadius: 4, fontSize: 12, fontWeight: 600, color: "#C0392B", cursor: clearing ? "not-allowed" : "pointer" }}>
            {clearing ? "Clearing…" : "✕ Clear"}
          </button>

          <button onClick={generateTimetable} disabled={generating} style={{ padding: "8px 18px", background: generating ? "#888" : "#2D3436", border: "none", borderRadius: 4, fontSize: 12, fontWeight: 700, color: "#FFFDF5", cursor: generating ? "not-allowed" : "pointer", letterSpacing: 0.5, transition: "background 0.2s" }}>
            {generating ? "Solving…" : "⚡ Generate Timetable"}
          </button>

          <button
            onClick={async () => {
              if (!gridRef.current) return;
              setDownloadingPdf(true);
              try {
                const canvas = await html2canvas(gridRef.current, {
                  scale: 2,
                  useCORS: true,
                  backgroundColor: "#F0EBE0",
                });
                const imgData = canvas.toDataURL("image/png");
                const pdf = new jsPDF({
                  orientation: "landscape",
                  unit: "px",
                  format: [canvas.width, canvas.height],
                });
                pdf.addImage(imgData, "PNG", 0, 0, canvas.width, canvas.height);
                const filename = `timetable_${departmentId}_${new Date().toISOString().slice(0, 10)}.pdf`;
                pdf.save(filename);
              } catch (err) {
                console.error("PDF export failed", err);
              } finally {
                setDownloadingPdf(false);
              }
            }}
            disabled={downloadingPdf || loading}
            style={{ padding: "8px 14px", background: "transparent", border: "1.5px solid #27AE60", borderRadius: 4, fontSize: 12, fontWeight: 600, color: "#27AE60", cursor: downloadingPdf ? "not-allowed" : "pointer" }}
          >
            {downloadingPdf ? "Exporting…" : "📄 Download PDF"}
          </button>
        </div>
      </div>

      {/* ── Editable Header Panel ── */}
      {editingHeader && (
        <div style={{ maxWidth: 1100, margin: "0 auto 16px", background: "#FFFDF5", border: "1.5px solid #C8C0A8", borderRadius: 4, padding: "16px 20px" }}>
          <p style={{ fontSize: 11, fontWeight: 700, color: "#8B7D6B", textTransform: "uppercase", letterSpacing: 1, margin: "0 0 12px" }}>Edit Timetable Header</p>
          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 12, marginBottom: 12 }}>
            <div>
              <label style={{ fontSize: 10, fontWeight: 700, color: "#8B7D6B", display: "block", marginBottom: 4 }}>INSTITUTION NAME</label>
              <input style={inputStyle} value={institutionName} onChange={e => setInstitutionName(e.target.value)} />
            </div>
            <div>
              <label style={{ fontSize: 10, fontWeight: 700, color: "#8B7D6B", display: "block", marginBottom: 4 }}>DEPARTMENT</label>
              <input style={inputStyle} value={departmentName} onChange={e => setDepartmentName(e.target.value)} />
            </div>
          </div>
          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: 12 }}>
            <div>
              <label style={{ fontSize: 10, fontWeight: 700, color: "#8B7D6B", display: "block", marginBottom: 4 }}>ACADEMIC YEAR</label>
              <input style={inputStyle} value={tableTitle} onChange={e => setTableTitle(e.target.value)} placeholder="e.g. TIME TABLE - 2024-25" />
            </div>
            <div>
              <label style={{ fontSize: 10, fontWeight: 700, color: "#8B7D6B", display: "block", marginBottom: 4 }}>SEMESTER / SECTION</label>
              <input style={inputStyle} value={semesterInfo} onChange={e => setSemesterInfo(e.target.value)} placeholder="e.g. I Semester Section – D (IS)" />
            </div>
            <div>
              <label style={{ fontSize: 10, fontWeight: 700, color: "#8B7D6B", display: "block", marginBottom: 4 }}>ROOM NO.</label>
              <input style={inputStyle} value={roomNo} onChange={e => setRoomNo(e.target.value)} placeholder="e.g. 452" />
            </div>
          </div>
        </div>
      )}

      {/* ── Grid + Editor Panel ── */}
      {loading ? (
        <div style={{ textAlign: "center", color: "#8B7D6B", marginTop: 80, fontSize: 14 }}>Loading timetable…</div>
      ) : (
        <div ref={gridRef}>
          <TimetableEditor
            key={gridKey}
            initialGrid={grid}
            departmentId={departmentId}
            legend={legend}
            onRefresh={fetchTimetable}
            institutionName={institutionName}
            departmentName={departmentName}
            tableTitle={tableTitle}
            semesterInfo={semesterInfo}
            roomNo={roomNo}
          />
        </div>
      )}
    </div>
  );
}