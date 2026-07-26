set pagination off
set confirm off
target extended-remote localhost:3333
monitor reset halt
load
set $pc = 0x20000001
continue
