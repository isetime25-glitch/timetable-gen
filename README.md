# Automated Timetable Generation System

> A full-stack academic scheduling system — built with Next.js, a C++ CP-SAT constraint solver (Google OR-Tools), and Supabase. Generates conflict-free timetables in under 10 seconds for an entire department.

---

## 📖 Overview

Manual timetable creation is error-prone, time-consuming, and difficult to update. This system automates the process by modelling scheduling as a **Constraint Satisfaction Problem** and solving it with Google OR-Tools' CP-SAT engine.

**Key highlights:**
- C++ solver enforcing **8 hard constraints** — no double-booking, lab-pair coupling, lunch boundaries, cross-section blocking, and more
- One-click timetable generation from an admin dashboard
- Drag-and-drop editor for manual adjustments with real-time collision detection
- Bulk data upload via CSV/PDF through a Python FastAPI micro-service
- Multi-stage Docker build, deployed on Railway

---

## 🏗️ System Architecture

```
┌────────────────────────────────────────────────────┐
│                 Client (Browser)                   │
│     Next.js 16 · React 19 · Tailwind CSS           │
│  Dashboard | Faculty | Subjects | Timetable        │
└──────────────────────┬─────────────────────────────┘
                       │ HTTPS
                       ▼
┌────────────────────────────────────────────────────┐
│           Next.js API Layer (Server-Side)          │
│  /api/schedule  /api/ingestion  /api/collisions    │
│                                                    │
│   solver-bridge.ts ──► spawn(scheduler binary)     │
│         │                      │                  │
│         ▼                      ▼                  │
│   Supabase Client       C++ CP-SAT Solver         │
└────────────────────────────────────────────────────┘
              │                         │
              ▼                         ▼
     Supabase Cloud DB         Python FastAPI Service
     (PostgreSQL)              (CSV / PDF ingestion)
```

The API layer fetches data from Supabase, pipes it to the C++ solver as JSON via `stdin`, and writes the result back to `timetable_slots`.

---

## 💻 Tech Stack

| Layer | Technology |
|---|---|
| Frontend Framework | Next.js 16.1 (React 19) |
| Language | TypeScript |
| Styling | Tailwind CSS + shadcn/ui + Radix UI |
| Animations | Framer Motion |
| Constraint Solver | C++ · Google OR-Tools CP-SAT v9.9 |
| Database / Auth | Supabase (PostgreSQL) |
| Ingestion Service | Python · FastAPI · Pandas |
| PDF Export | jsPDF + html2canvas-pro |
| Testing | Vitest |
| Containerisation | Docker (multi-stage) |
| Deployment | Railway |

---

## 🧠 Constraint Model — CP-SAT Solver

The solver reads a JSON payload, builds a CP model, and outputs the solution as JSON.

**Decision variable:** `X[teacher][subject][day][slot] = 1` if that slot is assigned.

**Hard Constraints (HC):**

| ID | Constraint |
|----|-----------|
| HC1 | No teacher double-booking per (day, slot) |
| HC1b | Single-section exclusivity per (day, slot) |
| HC3a | Exact weekly credit count per subject |
| HC4 | Teacher can only teach assigned subjects |
| HC5 | Slot pair (3, 4) forbidden — straddles lunch |
| HC6 | Max daily slots per teacher enforced |
| HC7 | Unavailable teachers blocked from all slots |
| HC8 | Lab subjects assigned consecutive pair slots only |
| HC9 | Cross-section teacher slots blocked via `blocked_slots` |

**Soft Constraint:** Schedule compactness — pushes free hours to end of day.

---

## 🚀 Getting Started

**Prerequisites:** Node.js v20+, Supabase project, Docker (for production)

```bash
# 1. Install dependencies
npm install

# 2. Set environment variables
cp .env.example .env.local
# Fill in NEXT_PUBLIC_SUPABASE_URL and NEXT_PUBLIC_SUPABASE_ANON_KEY

# 3. Run the dev server
npm run dev
```

Open [http://localhost:3000](http://localhost:3000) in your browser.

**Production:** Use the included multi-stage `Dockerfile` — it compiles the C++ solver, builds the Next.js bundle, and produces a slim runtime image.

---

## ✅ Testing Results

| Test Case | Result | Time |
|-----------|--------|------|
| Semester 3, Section A — ~25 slots | ✅ OPTIMAL | < 5s |
| Semester 3, Section B (with blocked cross-section slots) | ✅ OPTIMAL | < 8s |
| Semester 5, Section A — ~25 slots | ✅ OPTIMAL | < 5s |
| Invalid input (no teachers) | ✅ INFEASIBLE handled | < 1s |
| Solver timeout (1s limit, large dataset) | ✅ TIMEOUT handled | 1s |

```bash
npm run test        # Run all tests
npm run test:watch  # Watch mode
```

---

## 🔮 Future Scope

- **Multi-department support** — Schedule across departments with shared resource handling
- **Student elective preferences** — Factor student choices into scheduling
- **Room capacity as hard constraint** — Enforce during solve, not post-solve
- **Mobile companion app** — React Native app for faculty schedule viewing
- **LLM conflict resolution** — Suggest overrides when solver reports infeasibility
- **Exam scheduling** — Extend model to end-semester exams with invigilation duty

---

## 💬 Source Code

The C++ solver and full source are available in this repository. For questions or collaboration, reach out via the contact details on our profiles.

---

## 📄 License

Open for academic and personal use. Developed as a mini project at the **Department of Information Science & Engineering, MIT Mysore (2025–26)**.

> *Turning days of manual scheduling into seconds — one constraint at a time.*
