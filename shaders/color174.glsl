// --- 万花筒平铺 (Kaleidoscope Tiling) ---
// 融合了平铺、对称、形状、颜色与运动

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    // 1. 坐标标准化 (中心为0，保持宽高比)
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    
    // 时间变量 (控制动画速度)
    float t = iTime * 0.5;
    
    // 2. 运动元素：全局缓慢旋转
    float angle = t * 0.2;
    uv = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * uv;
    
    // 3. 平铺 (Tile)：放大坐标，产生网格
    uv *= 5.0; 
    
    // 获取当前像素所在的网格 ID 和 局部坐标(中心化)
    vec2 id = floor(uv);
    vec2 gv = fract(uv) - 0.5;
    
    // 4. 奇偶交错平铺：让相邻的格子上下翻转
    if (mod(id.x + id.y, 2.0) == 1.0) {
        gv.y = -gv.y;
    }
    
    // 5. 对称图案 (Symmetry)：利用绝对值折叠局部坐标
    // 仅使用右上角的四分之一空间来绘制
    gv = abs(gv);
    
    // --- 开始绘制形状 ---
    
    // 形状 A：绘制一个圆环 (基于距离)
    float d = length(gv - 0.2); // 将圆心移至(0.2, 0.2)
    float ring = smoothstep(0.12, 0.1, d) - smoothstep(0.05, 0.03, d);
    
    // 形状 B：绘制一个圆弧 (基于反正切角度)
    float a = atan(gv.y, gv.x);
    float arc = smoothstep(0.02, 0.0, abs(d - 0.25)) * step(a, 1.57);
    
    // 组合形状
    float shape = clamp(ring + arc, 0.0, 1.0);
    
    // --- 颜色映射 ---
    // 使用全局坐标计算流光溢彩的颜色
    // 这种余弦调色板是 Shadertoy 最经典的配色法
    vec3 palette = 0.5 + 0.5 * cos(t + uv.x + uv.y + vec3(0.0, 2.0, 4.0));
    
    // 背景设为深色，凸显荧光图案
    vec3 bgColor = vec3(0.05, 0.05, 0.08);
    
    // 混合背景、形状与调色板
    vec3 finalCol = mix(bgColor, palette, shape);
    
    // 增加发光效果 (让图案边缘更亮)
    finalCol += palette * shape * 0.5;
    
    // 增加暗角效果 (边缘变暗，聚焦中心)
    finalCol *= 1.0 - 0.5 * dot(uv * 0.05, uv * 0.05);
    
    fragColor = vec4(finalCol, 1.0);
}
