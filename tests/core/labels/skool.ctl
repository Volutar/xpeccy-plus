; A SkoolKit control file in the shape of Richard Dymond's (written for this test)
@ $8000 label=MAIN
c $8000 The main loop
@ $8004 label=DATA
b $8004 Some data
  $8004,6,2
B $800A,4,1*2,2
t $800E A message
  $800E,4,3:B1
w $8012 A table of words
  $8012,4,2
L $8012,4,2
i $801A
b $C000 The last block runs to the end; 010 is ten
  $C000,010,5
