# Security policy

Pip handles family voice messages, so security reports get priority
attention even though this is a spare-time project.

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub's security
advisory form:

**[Report a vulnerability](https://github.com/jpmreis/pipvoice/security/advisories/new)**

(Repository → Security tab → "Report a vulnerability".)

Please do **not** open a public issue for security problems, and please
do not test against Pip servers you do not operate yourself — the
server is self-hostable, so spin up your own instance
(`server/SETUP-LOCAL-MAC.md` gets one running locally in minutes).

## What to include

- Affected component (firmware / server / PWA) and version
  (`PROJECT_VER`, or a commit hash).
- Steps to reproduce, ideally against a local instance.
- Impact as you understand it.

## What to expect

- Acknowledgement within **7 days**, usually much sooner.
- An assessment and, for confirmed issues, a fix plan. Fixes ship as a
  regular release; the advisory is published after the fix is out.
- Credit in the advisory if you want it. There is no bug bounty.

## Scope notes

- The threat model and known accepted gaps are documented in
  `server/README.md` — issues listed there as accepted trade-offs
  (e.g. single-worker in-memory rate limiting) are still worth
  reporting if you can show real-world impact beyond what's described.
- Vulnerabilities in dependencies (ESP-IDF, LVGL, libopus, FastAPI,
  Mosquitto, Caddy) should go to those projects; a report here is
  appropriate if Pip's use of them is what creates the exposure.
