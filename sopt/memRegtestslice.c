asm (
".section	.text /*[SLICE_EXTRA] comes with 00000000*/\n"
".globl _section1 /*[SLICE_EXTRA] comes with 00000000*/\n"
"_section1: /*[SLICE_EXTRA] comes with 00000000*/\n"
"add edx, 91 /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"mov eax, 55 /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"mov double word ptr [0xbfffef74], 2519 /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"cmovs esp, eax /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"mov edi, byte ptr [0xbfffef75] /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"sub word ptr [0xbfffef74], esp /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"mov ebx, word ptr [0xbfffef74] /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"mov eax, 177 /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"sub eax, ebx /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"div double word ptr [0xbfffef74] /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"	
"div cx /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"add eax, 53 /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"add edx, edi /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"		
"mov ah, 25 /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"mov esp, edi /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"add esp, eax /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"cmovbe esp, eax /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"xadd eax, esp /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"
"xchg double word ptr [0xbfffef74], eax /*[SLICE] #00000000 [SLICE_INFO] comes with b7e8c19a*/\n"	
);
