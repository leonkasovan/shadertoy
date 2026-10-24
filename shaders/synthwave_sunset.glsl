// Synthwave sunset — striped sun, layered ridge silhouettes and an
// infinite neon grid. Pure 2D: gradients, value noise and a fake
// perspective floor, so it runs at a very high frame rate.

#define HORIZON 0.02

float hash21(vec2 p){
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float hash11(float n){ return fract(sin(n * 127.1) * 43758.5453); }

float noise1(float x){
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash11(i), hash11(i + 1.0), f);
}

float ridge(float x, float scale, float seed){
    float s = 0.0, amp = 0.55, f = scale;
    for (int i = 0; i < 4; i++){
        s += amp * noise1(x * f + seed);
        f *= 2.13;
        amp *= 0.5;
    }
    return s;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime;

    // ---------------- sky ----------------
    float k = smoothstep(HORIZON, 0.85, uv.y);
    vec3 col = mix(vec3(0.36, 0.05, 0.44), vec3(0.02, 0.01, 0.10), k);
    col += vec3(0.20, 0.02, 0.25) * exp(-max(uv.y - HORIZON, 0.0) * 4.0) * 0.6;

    // ---------------- stars ----------------
    vec2 sp = floor(uv * 48.0 + 0.5);
    vec2 sf = fract(uv * 48.0 + 0.5) - 0.5;
    float sh = hash21(sp);
    float star = smoothstep(0.972, 1.0, sh) * exp(-dot(sf, sf) * 26.0);
    star *= smoothstep(0.15, 0.5, uv.y);
    star *= 0.7 + 0.3 * sin(t * 3.0 + sh * 60.0);
    col += vec3(0.85, 0.90, 1.00) * star;

    // ---------------- sun ----------------
    vec2 sunP = vec2(0.0, 0.22);
    float sunR = 0.35;
    float sd = length(uv - sunP);
    float rel = (uv.y - sunP.y) / sunR;                 // -1..1 across disk
    float g = clamp(rel * 0.5 + 0.5, 0.0, 1.0);         // 0 bottom, 1 top

    // Yellow top -> orange middle -> hot pink base.
    vec3 sunCol = mix(vec3(1.00, 0.12, 0.42), vec3(1.00, 0.92, 0.30),
                      smoothstep(0.06, 0.85, g));

    float disk = 1.0 - smoothstep(sunR - 0.004, sunR, sd);
    float bands = smoothstep(0.20, 0.55, sin(rel * 42.0 - t * 2.0));
    float cut = smoothstep(0.10, 0.55, -rel);           // stripes widen downward
    disk *= 1.0 - 0.95 * bands * cut;

    // Bright but not clipped: the gradient has to stay readable.
    col = mix(col, sunCol * 1.25, disk);
    col += sunCol * exp(-sd * 4.5) * 0.22;
    col += vec3(1.0, 0.18, 0.45) * exp(-sd * 15.0) * 0.18;

    // ---------------- mountain silhouettes ----------------
    float above = smoothstep(HORIZON, HORIZON + 0.002, uv.y);

    float farH = 0.055 + 0.13 * ridge(uv.x * 0.9 + 3.0, 1.5, 4.0);
    float farMask = 1.0 - smoothstep(farH - 0.004, farH, uv.y - HORIZON);
    col = mix(col, vec3(0.16, 0.04, 0.28), farMask * above);

    float nearH = 0.025 + 0.075 * ridge(uv.x * 1.7 + 9.0, 2.2, 11.0);
    float nearMask = 1.0 - smoothstep(nearH - 0.004, nearH, uv.y - HORIZON);
    col = mix(col, vec3(0.035, 0.008, 0.10), nearMask * above);

    // ---------------- neon grid floor ----------------
    if (uv.y < HORIZON){
        float z = 1.0 / (HORIZON - uv.y + 0.004);       // fake depth
        float x = uv.x * z * 0.9;

        float cellZ = z * 0.55 + t * 2.6;               // scroll at viewer
        float cellX = x * 0.55;

        float lz = abs(fract(cellZ) - 0.5);
        float lx = abs(fract(cellX) - 0.5);
        float w = 0.030;
        float lines = smoothstep(0.5 - w, 0.5, lz)
                    + smoothstep(0.5 - w, 0.5, lx);

        float fade = exp(-z * 0.010);
        vec3 gridCol = mix(vec3(1.00, 0.12, 0.60), vec3(0.30, 0.80, 1.00),
                           smoothstep(0.0, 0.25, -uv.y));

        col = vec3(0.015, 0.0, 0.045) + gridCol * lines * fade * 1.5;
        col += vec3(1.0, 0.18, 0.50) * exp(-(HORIZON - uv.y) * 22.0) * 0.30;
    }

    col += vec3(1.0, 0.35, 0.60) * exp(-abs(uv.y - HORIZON) * 140.0) * 0.40;
    col *= 1.0 - 0.30 * dot(uv, uv);
    col = pow(max(col, 0.0), vec3(0.4545));
    fragColor = vec4(col, 1.0);
}
