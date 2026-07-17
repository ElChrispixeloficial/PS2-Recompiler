// PR2 Homebrew - Test Suite para PS2 Recompiler
// Este código se compila a MIPS y se ejecuta en el EE

void _start() {
    volatile unsigned int* GS_PRIM = (unsigned int*)0x12000000;
    volatile unsigned int* GS_RGBAQ = (unsigned int*)0x12000004;
    volatile unsigned int* GS_XYZ2 = (unsigned int*)0x12000014;
    
    // Configurar GS para dibujar un triángulo
    *GS_PRIM = 3;    // TRIANGLE
    *GS_RGBAQ = 0x80FF0000; // Rojo semi-transparente
    
    // Vértice 1: (100, 100, 0)
    *GS_XYZ2 = (100 << 0) | (100 << 16);
    *GS_XYZ2 = 0;
    
    // Vértice 2: (400, 100, 0)
    *GS_XYZ2 = (400 << 0) | (100 << 16);
    *GS_XYZ2 = 0;
    
    // Vértice 3: (250, 400, 0)
    *GS_XYZ2 = (250 << 0) | (400 << 16);
    *GS_XYZ2 = 0;
    
    // Bucle infinito
    while(1) {
        // Esperar VSYNC
        volatile unsigned int* GS_CSR = (unsigned int*)0x12001000;
        while (!(*GS_CSR & 8)); // Wait VSYNC
        *GS_CSR = 8; // Clear VSYNC
    }
}
