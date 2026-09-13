// ThirdPersonDot.fx - a small dot centred on the mouse position, for the Grim Dawn third-person runtime.
//
// While mouse look holds the cursor, the runtime keeps the mouse at the aim point, so this dot marks exactly where attacks
// and skills are aimed. It draws over the finished frame (after the game UI). It does not hide the game's own cursor; see
// docs/RESHADE_CURSOR.md for the optional Shader Toggler step that does.
//
// Self-contained: needs no other shader include and no texture.

uniform float2 MousePoint < source = "mousepoint"; >;

uniform float DotRadius <
    ui_type = "slider"; ui_min = 1.0; ui_max = 12.0; ui_step = 0.5;
    ui_label = "Dot radius (pixels)";
> = 3.0;

uniform float OutlineWidth <
    ui_type = "slider"; ui_min = 0.0; ui_max = 4.0; ui_step = 0.5;
    ui_label = "Outline width (pixels)";
> = 1.0;

uniform float OutlineOpacity <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Outline opacity";
> = 0.7;

uniform float3 DotColor <
    ui_type = "color";
    ui_label = "Dot colour";
> = float3(1.0, 1.0, 1.0);

texture ThirdPersonDotBackBuffer : COLOR;
sampler ThirdPersonDotBackBufferSampler { Texture = ThirdPersonDotBackBuffer; };

void ThirdPersonDotVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    // One full-screen triangle.
    uv.x = (id == 2) ? 2.0 : 0.0;
    uv.y = (id == 1) ? 2.0 : 0.0;
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 ThirdPersonDotPS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(ThirdPersonDotBackBufferSampler, uv);
    // position.xy is the pixel centre; MousePoint is the pixel the cursor is on.
    float distanceToMouse = distance(position.xy, MousePoint + 0.5);
    float dotCoverage = saturate(DotRadius + 0.5 - distanceToMouse);
    float outlineCoverage = saturate(DotRadius + OutlineWidth + 0.5 - distanceToMouse) * OutlineOpacity;
    color.rgb = lerp(color.rgb, float3(0.0, 0.0, 0.0), outlineCoverage * (1.0 - dotCoverage));
    color.rgb = lerp(color.rgb, DotColor, dotCoverage);
    return color;
}

technique ThirdPersonDot <
    ui_tooltip = "Draws a small dot at the mouse position (the third-person aim point while mouse look is active).";
>
{
    pass
    {
        VertexShader = ThirdPersonDotVS;
        PixelShader = ThirdPersonDotPS;
    }
}
