; module   = ULP2CM3.dll
; imagebase= 0x10000000
; range    = 0x10090CE0 - 0x10090DB0 (线性)

; ===== range 0x10090CE0-0x10090DB0 =====
0x10090CE0  55                       push ebp
0x10090CE1  8bec                     mov ebp, esp
0x10090CE3  83ec1c                   sub esp, 0x1c
0x10090CE6  a180e62910               mov eax, dword ptr [0x1029e680]
0x10090CEB  33c5                     xor eax, ebp
0x10090CED  8945fc                   mov dword ptr [ebp - 4], eax
0x10090CF0  803dcf3d2a1000           cmp byte ptr [0x102a3dcf], 0
0x10090CF7  8b5514                   mov edx, dword ptr [ebp + 0x14]
0x10090CFA  7539                     jne 0x10090d35
0x10090CFC  8b4510                   mov eax, dword ptr [ebp + 0x10]
0x10090CFF  8b4d0c                   mov ecx, dword ptr [ebp + 0xc]
0x10090D02  8945e8                   mov dword ptr [ebp - 0x18], eax
0x10090D05  0fb74508                 movzx eax, word ptr [ebp + 8]
0x10090D09  48                       dec eax
0x10090D0A  894de4                   mov dword ptr [ebp - 0x1c], ecx
0x10090D0D  83f803                   cmp eax, 3
0x10090D10  7723                     ja 0x10090d35
0x10090D12  ff2485980d0910           jmp dword ptr [eax*4 + 0x10090d98]
0x10090D19  8d4de4                   lea ecx, [ebp - 0x1c]
0x10090D1C  51                       push ecx
0x10090D1D  6a03                     push 3
0x10090D1F  a18c3c2a10               mov eax, dword ptr [0x102a3c8c]
0x10090D24  8955f4                   mov dword ptr [ebp - 0xc], edx
0x10090D27  8b15983c2a10             mov edx, dword ptr [0x102a3c98]
0x10090D2D  52                       push edx
0x10090D2E  50                       push eax
0x10090D2F  ff15e4882110             call dword ptr [0x102188e4]   ; call [0x102188E4]
0x10090D35  33c0                     xor eax, eax
0x10090D37  8b4dfc                   mov ecx, dword ptr [ebp - 4]
0x10090D3A  33cd                     xor ecx, ebp
0x10090D3C  e801df1500               call 0x101eec42   ; -> 0x101EEC42
0x10090D41  8be5                     mov esp, ebp
0x10090D43  5d                       pop ebp
0x10090D44  c3                       ret 
0x10090D45  8d4de4                   lea ecx, [ebp - 0x1c]
0x10090D48  51                       push ecx
0x10090D49  6a04                     push 4
0x10090D4B  ebd2                     jmp 0x10090d1f
0x10090D4D  83f903                   cmp ecx, 3
0x10090D50  74e3                     je 0x10090d35
0x10090D52  6a00                     push 0
0x10090D54  ff15b4882110             call dword ptr [0x102188b4]   ; call [0x102188B4]
0x10090D5A  33c0                     xor eax, eax
0x10090D5C  8b4dfc                   mov ecx, dword ptr [ebp - 4]
0x10090D5F  33cd                     xor ecx, ebp
0x10090D61  e8dcde1500               call 0x101eec42   ; -> 0x101EEC42
0x10090D66  8be5                     mov esp, ebp
0x10090D68  5d                       pop ebp
0x10090D69  c3                       ret 
0x10090D6A  a1943c2a10               mov eax, dword ptr [0x102a3c94]
0x10090D6F  c745ec01000000           mov dword ptr [ebp - 0x14], 1
0x10090D76  8955f4                   mov dword ptr [ebp - 0xc], edx
0x10090D79  85c0                     test eax, eax
0x10090D7B  74b8                     je 0x10090d35
0x10090D7D  8d4de4                   lea ecx, [ebp - 0x1c]
0x10090D80  51                       push ecx
0x10090D81  6a18                     push 0x18
0x10090D83  ffd0                     call eax
0x10090D85  8b4dfc                   mov ecx, dword ptr [ebp - 4]
0x10090D88  33cd                     xor ecx, ebp
0x10090D8A  83c408                   add esp, 8
0x10090D8D  e8b0de1500               call 0x101eec42   ; -> 0x101EEC42
0x10090D92  8be5                     mov esp, ebp
0x10090D94  5d                       pop ebp
0x10090D95  c3                       ret 
0x10090D96  8bff                     mov edi, edi
0x10090D98  190d0910450d             sbb dword ptr [0xd451009], ecx
0x10090D9E  0910                     or dword ptr [eax], edx
0x10090DA0  4d                       dec ebp
0x10090DA1  0d09106a0d               or eax, 0xd6a1009
0x10090DA6  0910                     or dword ptr [eax], edx
0x10090DA8  cc                       int3 
0x10090DA9  cc                       int3 
0x10090DAA  cc                       int3 
0x10090DAB  cc                       int3 
0x10090DAC  cc                       int3 
0x10090DAD  cc                       int3 
0x10090DAE  cc                       int3 
0x10090DAF  cc                       int3 

; ===== call graph (edges=5) =====
;  call -> 0x101EEC42
;  indirect [0x102188B4]
;  indirect [0x102188E4]
