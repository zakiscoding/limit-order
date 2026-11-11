# perf summary

Seed 42, 2,000,000 events, 5 pass(es) per run, 5 repeats per measurement. Counts are the loop run minus the setup-only run, divided by 10,000,000 applied events. Values are the median of the repeats with [min..max].

No hardware or software counter data was collected.

Allocation behavior (global operator new/delete, apply calls only):

| Measure | Value |
| --- | ---: |
| operator new calls | 684,912 |
| operator delete calls | 632,958 |
| bytes allocated (usable size) | 55,964,744 |
| peak live bytes above start (last pass) | 14,531,416 |
| live bytes above start at end of loop (last pass) | 11,315,640 |
| minor page faults in the loop | 2,052 |
| major page faults in the loop | 0 |
| peak RSS (KiB, whole process) | 93,452 |
| operator new calls per event | 0.3425 |

