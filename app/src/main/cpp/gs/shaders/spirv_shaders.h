#pragma once
#include <cstdint>

/* ---------------------------------------------------------------
 * Vertex shader SPIR-V 1.0 (precompilado)
 * GLSL fuente:
 *   #version 450
 *   layout(location=0) in  vec4 inPos;
 *   layout(location=1) in  vec4 inColor;
 *   layout(location=0) out vec4 fragColor;
 *   void main(){
 *       gl_Position = inPos;
 *       fragColor   = inColor;
 *   }
 * --------------------------------------------------------------- */
static const uint32_t VERT_SPIRV[] = {
    /* Magic, version 1.0, generator, bound=30, schema */
    0x07230203, 0x00010000, 0x00080001, 0x0000001E, 0x00000000,
    /* OpCapability Shader */
    0x00020011, 0x00000001,
    /* OpExtInstImport %1 "GLSL.std.450" */
    0x0006000B, 0x00000001, 0x4C534C47, 0x6474732E, 0x3534352E, 0x00000000,
    /* OpMemoryModel Logical GLSL450 */
    0x0003000E, 0x00000000, 0x00000001,
    /* OpEntryPoint Vertex %main "main" %inPos %inColor %fragColor %perVertex */
    0x000A000F, 0x00000000, 0x00000004, 0x6E69616D, 0x00000000,
                0x0000000B, 0x0000000D, 0x00000012, 0x00000016, 0x00000019,
    /* OpDecorate %inPos    Location 0 */
    0x00040047, 0x0000000B, 0x00000001, 0x00000000,
    /* OpDecorate %inColor  Location 1 */
    0x00040047, 0x0000000D, 0x00000001, 0x00000001,
    /* OpDecorate %fragColor Location 0 */
    0x00040047, 0x00000012, 0x00000001, 0x00000000,
    /* OpMemberDecorate %gl_PerVertex 0 BuiltIn Position */
    0x00050048, 0x00000015, 0x00000000, 0x0000000B, 0x00000000,
    /* OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize */
    0x00050048, 0x00000015, 0x00000001, 0x0000000B, 0x00000001,
    /* OpDecorate %gl_PerVertex Block */
    0x00030047, 0x00000015, 0x00000002,
    /* %void  = OpTypeVoid */
    0x00020013, 0x00000002,
    /* %func  = OpTypeFunction %void */
    0x00030021, 0x00000003, 0x00000002,
    /* %float = OpTypeFloat 32 */
    0x00030016, 0x00000006, 0x00000020,
    /* %v4f   = OpTypeVector %float 4 */
    0x00040017, 0x00000007, 0x00000006, 0x00000004,
    /* %ptrIn = OpTypePointer Input %v4f */
    0x00040020, 0x0000000A, 0x00000001, 0x00000007,
    /* %inPos    = OpVariable %ptrIn Input */
    0x0004003B, 0x0000000A, 0x0000000B, 0x00000001,
    /* %inColor  = OpVariable %ptrIn Input */
    0x0004003B, 0x0000000A, 0x0000000D, 0x00000001,
    /* %ptrOut = OpTypePointer Output %v4f */
    0x00040020, 0x00000011, 0x00000003, 0x00000007,
    /* %fragColor = OpVariable %ptrOut Output */
    0x0004003B, 0x00000011, 0x00000012, 0x00000003,
    /* %float1 = OpTypeFloat 32 (reuse) -- gl_PerVertex struct */
    /* %gl_PerVertex = OpTypeStruct %v4f %float */
    0x00040018, 0x00000015, 0x00000007, 0x00000006,
    /* %ptrOutBlock = OpTypePointer Output %gl_PerVertex */
    0x00040020, 0x00000017, 0x00000003, 0x00000015,
    /* %perVertex = OpVariable %ptrOutBlock Output */
    0x0004003B, 0x00000017, 0x00000019, 0x00000003,
    /* %int   = OpTypeInt 32 1 */
    0x00040015, 0x0000001A, 0x00000020, 0x00000001,
    /* %int_0 = OpConstant %int 0 */
    0x0004002B, 0x0000001A, 0x0000001B, 0x00000000,
    /* %ptrOutV4 = OpTypePointer Output %v4f (for member access) */
    /* (reuse %ptrOut = %11) */
    /* %main  = OpFunction %void None %func */
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    /* %label = OpLabel */
    0x000200F8, 0x00000005,
    /* %v1    = OpLoad %v4f %inPos */
    0x0004003D, 0x00000007, 0x0000001C, 0x0000000B,
    /* %v2    = OpLoad %v4f %inColor */
    0x0004003D, 0x00000007, 0x0000001D, 0x0000000D,
    /* OpStore %fragColor %v2 */
    0x0003003E, 0x00000012, 0x0000001D,
    /* %ptr   = OpAccessChain %ptrOut %perVertex %int_0 */
    0x00050041, 0x00000011, 0x0000001E, 0x00000019, 0x0000001B,
    /* OpStore %ptr %v1 */
    0x0003003E, 0x0000001E, 0x0000001C,
    /* OpReturn */
    0x000100FD,
    /* OpFunctionEnd */
    0x00010038
};
static const uint32_t VERT_SPIRV_SIZE = sizeof(VERT_SPIRV);

/* ---------------------------------------------------------------
 * Fragment shader SPIR-V 1.0 (precompilado)
 * GLSL fuente:
 *   #version 450
 *   layout(location=0) in  vec4 fragColor;
 *   layout(location=0) out vec4 outColor;
 *   void main(){ outColor = fragColor; }
 * --------------------------------------------------------------- */
static const uint32_t FRAG_SPIRV[] = {
    /* Magic, version 1.0, generator, bound=14, schema */
    0x07230203, 0x00010000, 0x00080001, 0x0000000E, 0x00000000,
    /* OpCapability Shader */
    0x00020011, 0x00000001,
    /* OpExtInstImport %1 "GLSL.std.450" */
    0x0006000B, 0x00000001, 0x4C534C47, 0x6474732E, 0x3534352E, 0x00000000,
    /* OpMemoryModel Logical GLSL450 */
    0x0003000E, 0x00000000, 0x00000001,
    /* OpEntryPoint Fragment %main "main" %fragColor %outColor */
    0x00080010, 0x00000004, 0x00000004, 0x6E69616D, 0x00000000,
                0x00000009, 0x0000000B, 0x00000000,
    /* OpExecutionMode %main OriginUpperLeft */
    0x00030010, 0x00000004, 0x00000008,
    /* OpDecorate %fragColor Location 0 */
    0x00040047, 0x00000009, 0x00000001, 0x00000000,
    /* OpDecorate %outColor  Location 0 */
    0x00040047, 0x0000000B, 0x00000001, 0x00000000,
    /* %void  = OpTypeVoid */
    0x00020013, 0x00000002,
    /* %func  = OpTypeFunction %void */
    0x00030021, 0x00000003, 0x00000002,
    /* %float = OpTypeFloat 32 */
    0x00030016, 0x00000006, 0x00000020,
    /* %v4f   = OpTypeVector %float 4 */
    0x00040017, 0x00000007, 0x00000006, 0x00000004,
    /* %ptrIn = OpTypePointer Input %v4f */
    0x00040020, 0x00000008, 0x00000001, 0x00000007,
    /* %fragColor = OpVariable %ptrIn Input */
    0x0004003B, 0x00000008, 0x00000009, 0x00000001,
    /* %ptrOut= OpTypePointer Output %v4f */
    0x00040020, 0x0000000A, 0x00000003, 0x00000007,
    /* %outColor = OpVariable %ptrOut Output */
    0x0004003B, 0x0000000A, 0x0000000B, 0x00000003,
    /* %main  = OpFunction %void None %func */
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    /* %label = OpLabel */
    0x000200F8, 0x00000005,
    /* %v     = OpLoad %v4f %fragColor */
    0x0004003D, 0x00000007, 0x0000000D, 0x00000009,
    /* OpStore %outColor %v */
    0x0003003E, 0x0000000B, 0x0000000D,
    /* OpReturn */
    0x000100FD,
    /* OpFunctionEnd */
    0x00010038
};
static const uint32_t FRAG_SPIRV_SIZE = sizeof(FRAG_SPIRV);
