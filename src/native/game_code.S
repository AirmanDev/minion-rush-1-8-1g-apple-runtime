.section __MRCODE,__text,regular,pure_instructions
.p2align 12
.globl _mr_game_code
_mr_game_code:
// The optional unsigned iOS template reserves file-backed executable pages.
// It contains NO pretranslated game instructions. The private AOT stage fills
// exactly this slot BEFORE a third-party signer signs the user's IPA.
#ifdef MR_IOS_AOT_TEMPLATE
#ifndef MR_AOT_SLOT_BYTES
#error "AOT template code slot size not configured"
#endif
.space MR_AOT_SLOT_BYTES, 0
#else
.incbin "build/minion_code.bin"
#endif
.globl _mr_game_code_end
_mr_game_code_end:
