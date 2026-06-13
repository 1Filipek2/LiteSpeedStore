# Records the LiteSpeedStore tamper-evidence demo to assets/demo.gif
# Build first, then run from the repo root:
#   cmake --build build --target litespeed-demo
#   vhs assets/demo.tape

Output assets/demo.gif

Set FontSize 18
Set Width 1000
Set Height 600
Set Padding 24
Set Theme "Dracula"

Hide
Type "clear" Enter
Show

Type "./build/litespeed-demo"
Sleep 800ms
Enter
Sleep 4s
