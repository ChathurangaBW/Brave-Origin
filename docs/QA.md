# QA evidence

Run `powershell -ExecutionPolicy Bypass -File .\qa-e2e.ps1`. The suite builds twice from clean outputs, compares byte hashes, and writes command transcripts, hashes, imports, and GUI process/window evidence to the ignored local `qa-results/` directory. A machine-neutral publication report is generated at [`qa/summary.md`](qa/summary.md).

The fixture `LOCALAPPDATA` is isolated under `qa-work`; apply and restore are never aimed at the real profile. Real-profile checks are read-only and guarded by start/end SHA-256 comparison and pre-existing Brave PID comparison. A mismatch is a hard suite failure.

Q1–Q12 cover help, missing channels, broken/ready checks, dry-run immutability, apply, marker verification, unrelated-key preservation, backup creation, malformed JSON, invalid options, restore byte round-trip, and deterministic collision suffixing. Adversarial tests cover strict RFC 8259 parsing, exact argument tokens versus decoys, allowlist containment, junction rejection, atomic-temp cleanup, and GUI double-start/close races. GUI shutdown uses an enumerated PID-owned HWND and `WM_CLOSE`; it never force-terminates the test process.
