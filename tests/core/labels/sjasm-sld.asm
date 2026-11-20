; sjasmplus --sld=sjasm-sld.sld sjasm-sld.asm (1.24.0) writes sjasm-sld.sld: labels with
; their 16K banks, every instruction's start, and the comments with DeZog's keywords
	DEVICE ZXSPECTRUM128
	SLDOPT COMMENT WPMEM, LOGPOINT, ASSERTION
CONST	EQU 3
	ORG #8000
start:	ld a,1		; ASSERTION A < 7
	ld hl,table
.loop:	ld (hl),a	; LOGPOINT [GAME] value=${A:hex8} at ${HL}
	inc hl
	jr .loop
table:	db 1,2,3	; WPMEM, 3, w
	MODULE gfx
draw:	ret
	ENDMODULE
	SLOT 3
	PAGE 7
	ORG #C123
b7:	nop
	nop
