# Sanitized QA report

Suite exit: **0**. Machine-specific paths, usernames, process/window identifiers, and real-profile hashes are intentionally omitted.

Source aggregate: `10F7824BC32602EC1C71D0FA1BF7D6FFB9EFE5A2BF9EEF5BC0E07FB09CD23FB0`
Malformed corpus: **14 / 14 rejected**.

| ID | Test | Result |
|---|---|---|
| B1 | Clean static release build | PASS |
| B2 | Reproducible byte-identical rebuild | PASS |
| TREE | Exact source aggregate captured | PASS |
| Q1 | Help and usage | PASS |
| Q2 | Missing channels are safe | PASS |
| Q3 | Broken check exit contract | PASS |
| Q4 | Dry-run exit contract | PASS |
| Q4H | Dry-run byte immutability | PASS |
| Q5 | Apply broken selected channel | PASS |
| Q6 | Ready check exit contract | PASS |
| Q7 | Unrelated JSON keys preserved | PASS |
| Q8 | Deterministic collision suffix and exact report | PASS |
| Q9 | Malformed JSON rejected | PASS |
| Q10 | Invalid channel rejected | PASS |
| Q11 | Valid backup restore | PASS |
| Q11H | Restore byte round-trip | PASS |
| Q12A | Second apply succeeds | PASS |
| Q12B | Third apply succeeds | PASS |
| Q12 | Backup names collision-safe | PASS |
| A1 | Internal parser/process-token self-test | PASS |
| A1O | Self-test assertions reported | PASS |
| A2 | RFC 8259 and RFC 3629 corpus rejected | PASS |
| A3 | Restore outside allowlist rejected | PASS |
| A4 | Junction/reparse restore rejected | PASS |
| A5 | Atomic operations leave no temp files | PASS |
| P-unreadable | Apply abort: unreadable candidate | PASS |
| P-unreadable-NW | No write/backup on unreadable abort | PASS |
| P-shutdown | Apply abort: shutdown candidate | PASS |
| P-shutdown-NW | No write/backup on shutdown abort | PASS |
| P-revalidation | Apply abort: revalidation candidate | PASS |
| P-revalidation-NW | No write/backup on revalidation abort | PASS |
| P-restore-unreadable | Restore abort: unidentified candidate | PASS |
| P-restore-unreadable-NW | Restore unidentified abort before backup/write | PASS |
| A6 | Restore TOCTOU identity/race abort | PASS |
| A6-NW | Restore race leaves target unchanged | PASS |
| A7 | Injected temp flush failure aborts apply | PASS |
| A7-NW | Flush failure leaves target/no temp residue | PASS |
| GUI | Window title and WM_CLOSE smoke | PASS |
| UI-RACE | Busy serialization and close guard | PASS |
| UI-CONTRACT | Confirmation/log/disclaimer/picker/DPI contract | PASS |
| REAL-HASH | Real Local State unchanged | PASS |
| REAL-PID | Pre-existing Brave PIDs unchanged | PASS |
| REAL-BAK | No real profile backups created | PASS |
| PROC | No residual GUI/CLI test process | PASS |

Two clean builds were byte-identical. Real-profile content, backup inventory, and pre-existing browser process set were unchanged. Raw evidence remains local under ignored `qa-results/`.
