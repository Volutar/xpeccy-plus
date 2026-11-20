|SLD.data.version|1
||K|KEYWORDS|WPMEM,LOGPOINT,ASSERTION
sjasm-sld.asm|3||0|-1|-1|Z|pages.size:16384,pages.count:8,slots.count:4,slots.adr:0,16384,32768,49152
sjasm-sld.asm|5||0|-1|3|D|CONST
sjasm-sld.asm|5||0|-1|3|L|,CONST,,+equ
sjasm-sld.asm|7||0|2|32768|K|; ASSERTION A < 7
sjasm-sld.asm|7||0|2|32768|F|start
sjasm-sld.asm|7||0|2|32768|L|,start,
sjasm-sld.asm|7||0|2|32768|T|
sjasm-sld.asm|8||0|2|32770|T|
sjasm-sld.asm|9||0|2|32773|K|; LOGPOINT [GAME] value=${A:hex8} at ${HL}
sjasm-sld.asm|9||0|2|32773|F|start.loop
sjasm-sld.asm|9||0|2|32773|L|,start,loop,+local,+used
sjasm-sld.asm|9||0|2|32773|T|
sjasm-sld.asm|10||0|2|32774|K|; WPMEM 32768, 2, w
sjasm-sld.asm|10||0|2|32774|T|
sjasm-sld.asm|11||0|2|32775|T|
sjasm-sld.asm|12||0|2|32777|K|; WPMEM, 3, w
sjasm-sld.asm|12||0|2|32777|F|table
sjasm-sld.asm|12||0|2|32777|L|,table,,+used
sjasm-sld.asm|13||0|-1|32780|L|gfx,,,+module
sjasm-sld.asm|14||0|2|32780|F|gfx.draw
sjasm-sld.asm|14||0|2|32780|L|gfx,draw,
sjasm-sld.asm|14||0|2|32780|T|
sjasm-sld.asm|15||0|-1|32781|L|gfx,,,+endmod
sjasm-sld.asm|19||0|7|49443|F|b7
sjasm-sld.asm|19||0|7|49443|L|,b7,
sjasm-sld.asm|19||0|7|49443|T|
sjasm-sld.asm|20||0|7|49444|T|
