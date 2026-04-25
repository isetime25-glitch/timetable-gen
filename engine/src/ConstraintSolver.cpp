// =============================================================================
// ConstraintSolver.cpp  v5 (HC2 removed)
// =============================================================================
//
// DECISION VARIABLE
//   X[t][s][d][p] = 1  iff teacher t teaches subject s on day d at slot p
//
// HARD CONSTRAINTS
//   HC1   No teacher double-booking: at most one subject per teacher per slot
//   HC1b  Single section: at most one subject per (day, slot) across all teachers
//         — prevents two classes running simultaneously for the same student group
//   HC2   REMOVED — rooms are assigned during extraction, not constrained during solving
//   HC3   Exact weekly slot count per subject (HC3a) +
//         exact lab pair count for lab subjects (HC3b)
//   HC4   Only valid teacher-subject assignments
//   HC5   Lunch boundary — pair (3,4) forbidden; all 6 slots are valid teaching slots
//   HC6   Max daily slots per teacher
//   HC7   Unavailable teachers blocked
//   HC8   Lab pair coupling: pair_start ↔ pair_start+1, at most 1 pair per day
//
// SOFT CONSTRAINTS
//   SC1   Cap slots per (subject, day) to avoid clustering
//
// ROOM ASSIGNMENT (extraction phase only):
//   - Lab pair slots  → lab_room_idx
//   - Lecture slots   → lecture_room_idx
//   - Mixed subjects: pair slots use lab, lecture slots use lecture room
//
// VTU SLOT LAYOUT (6 teaching slots, NO slot is blocked):
//   slot 0: 09:00-10:00
//   slot 1: 10:00-11:00
//   [TEA BREAK — visual only]
//   slot 2: 11:15-12:15
//   slot 3: 12:15-13:15
//   [LUNCH BREAK — visual boundary between slot 3 and slot 4]
//   slot 4: 14:00-15:00
//   slot 5: 15:00-16:00
//
//   Valid pair starts: {0, 2, 4}  →  pairs (0,1), (2,3), (4,5)
//   Forbidden pair:   (3,4) — straddles the lunch boundary
//
// =============================================================================

#include "ortools/sat/cp_model.h"
#include "ortools/sat/model.h"
#include "ortools/sat/sat_parameters.pb.h"

#include <vector>
#include <string>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace operations_research;
using namespace operations_research::sat;

// =============================================================================
// MINIMAL JSON PARSER
// =============================================================================

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue*>;
using JsonArray  = std::vector<JsonValue*>;

struct JsonValue {
    enum Type { STRING, NUMBER, BOOL_VAL, NULL_VAL, OBJECT, ARRAY } type;
    std::string str_val;
    double      num_val  = 0;
    bool        bool_val = false;
    JsonObject  obj_val;
    JsonArray   arr_val;

    ~JsonValue() {
        for (auto& kv : obj_val) delete kv.second;
        for (auto  v  : arr_val) delete v;
    }
    std::string getString(const std::string& key, const std::string& def = "") const {
        auto it = obj_val.find(key);
        if (it == obj_val.end() || it->second->type != STRING) return def;
        return it->second->str_val;
    }
    int getInt(const std::string& key, int def = 0) const {
        auto it = obj_val.find(key);
        if (it == obj_val.end() || it->second->type != NUMBER) return def;
        return (int)it->second->num_val;
    }
    bool getBool(const std::string& key, bool def = true) const {
        auto it = obj_val.find(key);
        if (it == obj_val.end() || it->second->type != BOOL_VAL) return def;
        return it->second->bool_val;
    }
    const JsonArray& getArray(const std::string& key) const {
        static JsonArray empty;
        auto it = obj_val.find(key);
        if (it == obj_val.end() || it->second->type != ARRAY) return empty;
        return it->second->arr_val;
    }
};

static void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\n'||s[i]=='\r'||s[i]=='\t')) i++;
}
static std::string parseString(const std::string& s, size_t& i) {
    i++;
    std::string r;
    while (i < s.size() && s[i] != '"') {
        if (s[i]=='\\') { i++; r += s[i]; } else r += s[i];
        i++;
    }
    i++;
    return r;
}
static JsonValue* parseValue(const std::string& s, size_t& i);
static JsonValue* parseObject(const std::string& s, size_t& i) {
    i++;
    auto* obj = new JsonValue(); obj->type = JsonValue::OBJECT;
    skipWs(s,i);
    while (i < s.size() && s[i] != '}') {
        skipWs(s,i);
        std::string key = parseString(s,i);
        skipWs(s,i); i++; skipWs(s,i);
        obj->obj_val[key] = parseValue(s,i);
        skipWs(s,i);
        if (i < s.size() && s[i]==',') i++;
        skipWs(s,i);
    }
    i++;
    return obj;
}
static JsonValue* parseArray(const std::string& s, size_t& i) {
    i++;
    auto* arr = new JsonValue(); arr->type = JsonValue::ARRAY;
    skipWs(s,i);
    while (i < s.size() && s[i] != ']') {
        arr->arr_val.push_back(parseValue(s,i));
        skipWs(s,i);
        if (i < s.size() && s[i]==',') i++;
        skipWs(s,i);
    }
    i++;
    return arr;
}
static JsonValue* parseValue(const std::string& s, size_t& i) {
    skipWs(s,i);
    if (s[i]=='"') { auto* v=new JsonValue(); v->type=JsonValue::STRING; v->str_val=parseString(s,i); return v; }
    if (s[i]=='{') return parseObject(s,i);
    if (s[i]=='[') return parseArray(s,i);
    if (s.substr(i,4)=="true")  { i+=4; auto* v=new JsonValue(); v->type=JsonValue::BOOL_VAL; v->bool_val=true;  return v; }
    if (s.substr(i,5)=="false") { i+=5; auto* v=new JsonValue(); v->type=JsonValue::BOOL_VAL; v->bool_val=false; return v; }
    if (s.substr(i,4)=="null")  { i+=4; auto* v=new JsonValue(); v->type=JsonValue::NULL_VAL; return v; }
    auto* v=new JsonValue(); v->type=JsonValue::NUMBER;
    size_t start=i;
    if (s[i]=='-') i++;
    while (i<s.size()&&(isdigit(s[i])||s[i]=='.'||s[i]=='e'||s[i]=='E'||s[i]=='+'||s[i]=='-')) i++;
    v->num_val=std::stod(s.substr(start,i-start));
    return v;
}

// =============================================================================
// DATA STRUCTURES
// =============================================================================

struct Teacher { std::string id, name; bool is_available; int max_daily_slots; };
struct Subject {
    std::string id, code, preferred_room_type;
    int weekly_credits;
    int practical_hours;
};
struct Room    { std::string id, room_name, room_type; int capacity; };
struct Assignment { std::string teacher_id, subject_id; };
struct BlockedSlot { std::string teacher_id; int day, slot; };

struct ScheduleInput {
    std::string department_id;
    std::vector<Teacher>    teachers;
    std::vector<Subject>    subjects;
    std::vector<Room>       rooms;
    std::vector<Assignment> assignments;
    std::vector<BlockedSlot> blocked_slots;
    int num_days, slots_per_day, lunch_slot_index, time_limit_seconds;
};

struct TimetableSlot {
    std::string teacher_id, subject_id, room_id;
    int         day_of_week, slot_index;
    std::string slot_status, conflict_reason;
    bool        is_lab, is_double_start;
};

struct ScheduleOutput {
    bool        success;
    std::string status;
    std::vector<TimetableSlot> slots;
    std::vector<std::string>   conflict_log;
};

// =============================================================================
// PARSE INPUT
// =============================================================================

ScheduleInput parseInput(const std::string& js) {
    size_t i=0;
    JsonValue* root=parseValue(js,i);
    ScheduleInput inp;
    inp.department_id      = root->getString("department_id");
    inp.num_days           = root->getInt("days_per_week",      5);
    inp.slots_per_day      = root->getInt("slots_per_day",      6);
    inp.lunch_slot_index   = root->getInt("lunch_slot_index",   3);
    inp.time_limit_seconds = root->getInt("time_limit_seconds", 30);
    for (auto* v : root->getArray("teachers"))
        inp.teachers.push_back({v->getString("id"), v->getString("name"),
                                v->getBool("is_available",true),
                                v->getInt("max_daily_slots",4)});
    for (auto* v : root->getArray("subjects"))
        inp.subjects.push_back({v->getString("id"), v->getString("code"),
                                v->getString("preferred_room_type","lecture"),
                                v->getInt("weekly_credits",1),
                                v->getInt("practical_hours",0)});
    for (auto* v : root->getArray("rooms"))
        inp.rooms.push_back({v->getString("id"), v->getString("room_name"),
                             v->getString("room_type","lecture"),
                             v->getInt("capacity",60)});
    for (auto* v : root->getArray("assignments"))
        inp.assignments.push_back({v->getString("teacher_id"),
                                   v->getString("subject_id")});
    for (auto* v : root->getArray("blocked_slots"))
        inp.blocked_slots.push_back({v->getString("teacher_id"),
                                     v->getInt("day"),
                                     v->getInt("slot")});
    delete root;
    return inp;
}

// =============================================================================
// SOLVER
// =============================================================================

class TimetableSolver {
public:
    ScheduleOutput Solve(const ScheduleInput& inp) {
        CpModelBuilder model;

        const int T = (int)inp.teachers.size();
        const int S = (int)inp.subjects.size();
        const int R = (int)inp.rooms.size();
        const int D = inp.num_days;
        const int P = inp.slots_per_day;

        if (T==0 || S==0) {
            ScheduleOutput o; o.success=false; o.status="INFEASIBLE";
            o.conflict_log.push_back("No teachers or subjects provided.");
            return o;
        }

        // ── Decision variables ────────────────────────────────────────────────
        std::vector<std::vector<std::vector<std::vector<BoolVar>>>> X(
            T, std::vector<std::vector<std::vector<BoolVar>>>(
            S, std::vector<std::vector<BoolVar>>(
            D, std::vector<BoolVar>(P))));
        for (int t=0;t<T;t++) for (int s=0;s<S;s++)
            for (int d=0;d<D;d++) for (int p=0;p<P;p++)
                X[t][s][d][p] = model.NewBoolVar();

        // ── Valid pair start positions ─────────────────────────────────────
        std::vector<int> VPS;
        {
            int p=0;
            while (p < P-1) {
                if (p == inp.lunch_slot_index) { p++; continue; }
                VPS.push_back(p);
                p += 2;
            }
        }
        std::set<int> PSS;
        for (int ps : VPS) { PSS.insert(ps); PSS.insert(ps+1); }

        // ── Room index lookup ──────────────────────────────────────────────
        int lecture_room_idx = -1;
        int lab_room_idx     = -1;
        for (int r=0; r<R; r++) {
            if (lecture_room_idx < 0 && inp.rooms[r].room_type == "lecture")
                lecture_room_idx = r;
            if (lab_room_idx < 0 && inp.rooms[r].room_type == "lab")
                lab_room_idx = r;
        }
        std::vector<int> subj_room(S, -1);
        for (int s=0; s<S; s++)
            for (int r=0; r<R; r++)
                if (inp.rooms[r].room_type == inp.subjects[s].preferred_room_type)
                    { subj_room[s] = r; break; }

        // ── HC1: No teacher double-booking ────────────────────────────────
        for (int t=0;t<T;t++) for (int d=0;d<D;d++) for (int p=0;p<P;p++) {
            std::vector<BoolVar> v;
            for (int s=0;s<S;s++) v.push_back(X[t][s][d][p]);
            model.AddAtMostOne(v);
        }

        // ── HC1b: Single section constraint ───────────────────────────────
        for (int d=0;d<D;d++) for (int p=0;p<P;p++) {
            std::vector<BoolVar> v;
            for (int t=0;t<T;t++) for (int s=0;s<S;s++) v.push_back(X[t][s][d][p]);
            model.AddAtMostOne(v);
        }

        // ── HC2: REMOVED ──────────────────────────────────────────────────
        // Room assignment is handled during extraction, not during constraint solving.
        // HC1b already ensures no slot conflicts, so room capacity is never an issue.

        // ── HC3: Weekly slot count (HC3b moved to HC8) ──────────────────────
        // HC3a: total assigned slots == weekly_credits
        // HC3b (pair-start count) is now enforced inside HC8 via L-sum.
        for (int s=0;s<S;s++) {
            std::vector<BoolVar> all;
            for (int t=0;t<T;t++) for (int d=0;d<D;d++) for (int p=0;p<P;p++)
                all.push_back(X[t][s][d][p]);
            model.AddEquality(LinearExpr::Sum(all), inp.subjects[s].weekly_credits);
        }

        // ── HC4: Valid teacher-subject assignments only ───────────────────
        std::set<std::pair<int,int>> VT;
        for (const auto& a : inp.assignments) {
            int ti=-1, si=-1;
            for (int i=0;i<T;i++) if (inp.teachers[i].id==a.teacher_id) { ti=i; break; }
            for (int i=0;i<S;i++) if (inp.subjects[i].id==a.subject_id)  { si=i; break; }
            if (ti>=0 && si>=0) VT.insert({ti,si});
        }
        for (int t=0;t<T;t++) for (int s=0;s<S;s++)
            if (!VT.count({t,s}))
                for (int d=0;d<D;d++) for (int p=0;p<P;p++)
                    model.FixVariable(X[t][s][d][p], false);

        // ── HC5: Lunch boundary ───────────────────────────────────────────
        // All 6 slots are valid. Forbidden pair (3,4) is excluded from VPS.

        // ── HC6: Max daily slots per teacher ─────────────────────────────
        for (int t=0;t<T;t++) for (int d=0;d<D;d++) {
            std::vector<BoolVar> v;
            for (int s=0;s<S;s++) for (int p=0;p<P;p++) v.push_back(X[t][s][d][p]);
            model.AddLessOrEqual(LinearExpr::Sum(v), inp.teachers[t].max_daily_slots);
        }

        // ── HC7: Unavailable teachers blocked ────────────────────────────
        for (int t=0;t<T;t++)
            if (!inp.teachers[t].is_available)
                for (int s=0;s<S;s++) for (int d=0;d<D;d++) for (int p=0;p<P;p++)
                    model.FixVariable(X[t][s][d][p], false);

        // ── HC9: Cross-section teacher blocking ──────────────────────────
        for (const auto& bs : inp.blocked_slots) {
            int ti = -1;
            for (int i = 0; i < T; i++)
                if (inp.teachers[i].id == bs.teacher_id) { ti = i; break; }
            if (ti >= 0 && bs.day >= 0 && bs.day < D && bs.slot >= 0 && bs.slot < P)
                for (int s = 0; s < S; s++)
                    model.FixVariable(X[ti][s][bs.day][bs.slot], false);
        }

        // ── HC8: Lab pair coupling (with auxiliary L variables) ─────────────
        //
        // L[t][s][d][ps] = 1 iff teacher t teaches lab subject s as a
        // consecutive pair on day d starting at slot ps.
        //
        //   L=1 → X[ps]=1 AND X[ps+1]=1      (pair forces both slots)
        //   L=0 → NOT(X[ps]=1 AND X[ps+1]=1) (no accidental adjacent lectures)
        //   Sum(L for subject s) = practical_hours / 2  (replaces HC3b)
        //   AtMostOne(L per day per teacher-subject)    (max 1 pair per day)
        //
        // Lecture slots of lab subjects are free to go at ANY position.
        //
        for (int s=0;s<S;s++) {
            if (inp.subjects[s].practical_hours <= 0) continue;
            int num_pairs = inp.subjects[s].practical_hours / 2;

            std::vector<BoolVar> all_L;  // all L vars for this subject

            for (int t=0;t<T;t++) for (int d=0;d<D;d++) {
                std::vector<BoolVar> day_L;  // L vars for this (t,s,d)

                for (int ps : VPS) {
                    BoolVar L = model.NewBoolVar();

                    // L → X[ps]  (pair start forces teaching at ps)
                    model.AddImplication(L, X[t][s][d][ps]);
                    // L → X[ps+1]  (pair start forces teaching at ps+1)
                    model.AddImplication(L, X[t][s][d][ps+1]);
                    // ¬L → ¬(X[ps] ∧ X[ps+1])  (no accidental pair)
                    // Equivalent: L ∨ ¬X[ps] ∨ ¬X[ps+1]
                    model.AddBoolOr({L, X[t][s][d][ps].Not(),
                                     X[t][s][d][ps+1].Not()});

                    all_L.push_back(L);
                    day_L.push_back(L);
                }

                // At most one pair start per (teacher, subject, day)
                model.AddAtMostOne(day_L);
            }

            // Exact number of lab pairs for this subject (replaces HC3b)
            model.AddEquality(LinearExpr::Sum(all_L), num_pairs);

            // Prevent break-straddling adjacencies for lab subjects.
            // Adjacent slots that are NOT valid pairs (e.g. 1→2 across tea break,
            // 3→4 across lunch) must not both be assigned — they would look like
            // a broken lab pair in the timetable.
            for (int t=0;t<T;t++) for (int d=0;d<D;d++)
                for (int p=0; p<P-1; p++) {
                    // Skip valid pair starts — those are handled by L variables
                    bool is_vps = false;
                    for (int ps : VPS) if (ps == p) { is_vps = true; break; }
                    if (is_vps) continue;
                    // Prevent both X[p] and X[p+1] from being true
                    model.AddBoolOr({X[t][s][d][p].Not(),
                                     X[t][s][d][p+1].Not()});
                }
        }

        // ── SC1: Even distribution ────────────────────────────────────────
        // for (int s=0;s<S;s++) {
        //     int cap = (inp.subjects[s].weekly_credits + D - 1) / D;
        //     if (inp.subjects[s].practical_hours > 0) {
        //         cap = std::max(cap, 3);  // Allow pair + lecture on same day
        //     }
        //     for (int d=0;d<D;d++) {
        //         std::vector<BoolVar> v;
        //         for (int t=0;t<T;t++) for (int p=0;p<P;p++) v.push_back(X[t][s][d][p]);
        //         model.AddLessOrEqual(LinearExpr::Sum(v), cap);
        //     }
        // }

        // ── SC2: Compactness — push free hours to end of day ─────────────
        // For each day, if slot p+1 is occupied then slot p should also be
        // occupied.  Implemented as a soft constraint via maximisation so it
        // cannot cause infeasibility.
        LinearExpr compactness_bonus;
        for (int d=0;d<D;d++) {
            // any_slot[d][p] = true iff at least one (t,s) is assigned at (d,p)
            std::vector<BoolVar> any_slot(P);
            for (int p=0;p<P;p++) {
                any_slot[p] = model.NewBoolVar();
                std::vector<BoolVar> slot_usages;
                for (int t=0;t<T;t++) for (int s=0;s<S;s++)
                    slot_usages.push_back(X[t][s][d][p]);
                // any_slot[p] == 1 iff at least one X[t][s][d][p] == 1
                model.AddBoolOr(slot_usages).OnlyEnforceIf(any_slot[p]);
                for (auto& xu : slot_usages)
                    model.AddImplication(xu, any_slot[p]);
                // if none are assigned, any_slot must be false
                std::vector<BoolVar> negated;
                for (auto& xu : slot_usages) negated.push_back(xu.Not());
                negated.push_back(any_slot[p].Not());
                // At least one of: some slot assigned, or any_slot=false
                // This is already implied by the above two groups.
            }

            // Bonus: reward filling earlier slots (higher weight for earlier)
            for (int p=0;p<P;p++) {
                // Weight: earlier slots get higher bonus
                int weight = (P - p);
                compactness_bonus += weight * any_slot[p];
            }
        }
        model.Maximize(compactness_bonus);


        // ── Solve ─────────────────────────────────────────────────────────
        Model sat_model;
        SatParameters params;
        params.set_max_time_in_seconds((double)inp.time_limit_seconds);
        params.set_num_search_workers(1);
        params.set_log_search_progress(false);
        params.set_cp_model_presolve(true);
        sat_model.Add(NewSatParameters(params));
        const CpSolverResponse resp = SolveCpModel(model.Build(), &sat_model);

        ScheduleOutput out;
        out.success = (resp.status()==CpSolverStatus::OPTIMAL ||
                       resp.status()==CpSolverStatus::FEASIBLE);
        if      (resp.status()==CpSolverStatus::OPTIMAL)    out.status="OPTIMAL";
        else if (resp.status()==CpSolverStatus::FEASIBLE)   out.status="FEASIBLE";
        else if (resp.status()==CpSolverStatus::INFEASIBLE) {
            out.status="INFEASIBLE";
            out.conflict_log.push_back(
                "INFEASIBLE: Check weekly_credits, practical_hours parity, "
                "teacher assignments, max_daily_slots, and room pool.");
            return out;
        } else {
            out.status="UNKNOWN";
            out.conflict_log.push_back("Solver timed out or returned unknown status.");
            return out;
        }

        // ── Extract solution ───────────────────────────────────────────────
        std::set<std::tuple<int,int,int,int>> second_of_pair;
        for (int t=0;t<T;t++) for (int s=0;s<S;s++) {
            if (inp.subjects[s].practical_hours <= 0) continue;
            for (int d=0;d<D;d++) for (int ps : VPS)
                if (SolutionBooleanValue(resp, X[t][s][d][ps]) &&
                    SolutionBooleanValue(resp, X[t][s][d][ps+1]))
                    second_of_pair.insert({t,s,d,ps+1});
        }

        for (int t=0;t<T;t++) for (int s=0;s<S;s++)
            for (int d=0;d<D;d++) for (int p=0;p<P;p++) {
                if (!SolutionBooleanValue(resp, X[t][s][d][p])) continue;

                TimetableSlot slot;
                slot.teacher_id  = inp.teachers[t].id;
                slot.subject_id  = inp.subjects[s].id;
                slot.day_of_week = d + 1;
                slot.slot_index  = p;
                slot.slot_status = "scheduled";

                bool lab_sub    = inp.subjects[s].practical_hours > 0;
                bool in_pair    = PSS.count(p) > 0;
                bool is_second  = second_of_pair.count({t,s,d,p}) > 0;
                bool is_first   = lab_sub && in_pair && !is_second
                                  && (p+1 < P)
                                  && SolutionBooleanValue(resp, X[t][s][d][p+1]);

                slot.is_lab          = is_first || is_second;
                slot.is_double_start = is_first;

                // Room assignment based on actual pair status
                int room_idx;
                if (slot.is_lab) {
                    room_idx = lab_room_idx;
                } else {
                    room_idx = (lecture_room_idx >= 0) ? lecture_room_idx : subj_room[s];
                }

                if (room_idx >= 0) {
                    slot.room_id = inp.rooms[room_idx].id;
                } else {
                    slot.room_id         = "";
                    slot.slot_status     = "pending_room";
                    slot.conflict_reason = "No room found for: "
                                         + inp.subjects[s].preferred_room_type;
                    out.conflict_log.push_back(
                        "Subject " + inp.subjects[s].code + " has no matching room.");
                }

                out.slots.push_back(slot);
            }

        return out;
    }
};

// =============================================================================
// JSON OUTPUT
// =============================================================================

static std::string esc(const std::string& s) {
    std::ostringstream o;
    for (char c : s) switch(c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n";  break;
        case '\r': o << "\\r";  break;
        case '\t': o << "\\t";  break;
        default:   o << c;
    }
    return o.str();
}

static std::string toJson(const ScheduleOutput& out) {
    std::ostringstream j;
    j << "{\n  \"success\": " << (out.success?"true":"false") << ",\n";
    j << "  \"status\": \""   << esc(out.status) << "\",\n";
    j << "  \"slots\": [\n";
    for (size_t i=0; i<out.slots.size(); i++) {
        const auto& s = out.slots[i];
        j << "    {\n";
        j << "      \"teacher_id\": \""    << esc(s.teacher_id)    << "\",\n";
        j << "      \"subject_id\": \""    << esc(s.subject_id)    << "\",\n";
        j << "      \"room_id\": \""       << esc(s.room_id)       << "\",\n";
        j << "      \"day_of_week\": "     << s.day_of_week        << ",\n";
        j << "      \"slot_index\": "      << s.slot_index         << ",\n";
        j << "      \"slot_status\": \""   << esc(s.slot_status)   << "\",\n";
        j << "      \"is_lab\": "          << (s.is_lab          ? "true":"false") << ",\n";
        j << "      \"is_double_start\": " << (s.is_double_start ? "true":"false") << ",\n";
        j << "      \"metadata\": { \"conflict_reason\": \""
          << esc(s.conflict_reason) << "\" }\n";
        j << "    }" << (i+1<out.slots.size()?",":"") << "\n";
    }
    j << "  ],\n  \"conflict_log\": [\n";
    for (size_t i=0; i<out.conflict_log.size(); i++)
        j << "    \"" << esc(out.conflict_log[i]) << "\""
          << (i+1<out.conflict_log.size()?",":"") << "\n";
    j << "  ]\n}";
    return j.str();
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    std::string line, input;
    while (std::getline(std::cin, line)) input += line + "\n";
    if (input.empty()) {
        std::cout << "{\"success\":false,\"status\":\"ERROR\",\"slots\":[],"
                     "\"conflict_log\":[\"No input received\"]}" << std::endl;
        return 1;
    }
    try {
        ScheduleInput   si  = parseInput(input);
        TimetableSolver solver;
        ScheduleOutput  out = solver.Solve(si);
        std::cout << toJson(out) << std::endl;
        return out.success ? 0 : 1;
    } catch (const std::exception& e) {
        std::cout << "{\"success\":false,\"status\":\"ERROR\",\"slots\":[],"
                     "\"conflict_log\":[\"" << esc(e.what()) << "\"]}" << std::endl;
        return 1;
    }
}