// Neon wormhole — an endless twisting tunnel of glowing rails.
// Raymarched inside a warped cylinder; panels and rails are emissive,
// with a soft volumetric halo accumulated during the march.

#define TAU 6.28318530718
#define STEPS 96

mat2 rot2(float a){ float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }

// The tunnel twists more strongly with depth, so the far end spirals away.
float twistAt(float z){
    return 0.42 * sin(z * 0.15 - iTime * 0.55) + z * 0.06;
}

// Wall radius: angular ripples that scroll toward the camera.
float wallRadius(float ang, float z){
    return 1.55
         + 0.16 * sin(ang * 5.0 + z * 1.15 - iTime * 1.9)
         + 0.08 * sin(ang * 9.0 - z * 2.40 + iTime * 2.7);
}

// Positive inside the tunnel, negative outside.
float map(vec3 p){
    vec2 q = rot2(twistAt(p.z)) * p.xy;
    float l = length(q);
    return wallRadius(atan(q.y, q.x), p.z) - l;
}

vec3 calcNormal(vec3 p){
    vec2 e = vec2(0.0025, 0.0);
    return normalize(vec3(
        map(p + e.xyy) - map(p - e.xyy),
        map(p + e.yxy) - map(p - e.yxy),
        map(p + e.yyx) - map(p - e.yyx)));
}

vec3 shade(vec3 p, vec3 rd, float t){
    vec2 q = rot2(twistAt(p.z)) * p.xy;
    float u = atan(q.y, q.x) / TAU + 0.5;      // 0..1 around the tunnel
    float v = p.z * 0.45 - iTime * 0.6;        // scrolling depth coordinate

    // Thin emissive rails at the panel borders.
    float ru = abs(fract(u * 10.0) - 0.5);
    float rv = abs(fract(v * 1.5) - 0.5);
    float rail = smoothstep(0.455, 0.5, ru) + smoothstep(0.450, 0.5, rv);
    rail = clamp(rail, 0.0, 1.0);

    float panel = 0.5 + 0.5 * sin(u * TAU * 3.0 + v * 2.0);

    vec3 base = mix(vec3(0.010, 0.020, 0.055), vec3(0.030, 0.008, 0.055), panel);
    vec3 railCol = mix(vec3(0.15, 0.95, 1.00), vec3(1.00, 0.10, 0.70),
                       0.5 + 0.5 * sin(u * TAU * 2.0 - iTime * 0.8));

    vec3 n = -calcNormal(p);                   // inward wall normal
    vec3 l = normalize(vec3(0.35, 0.75, -0.45));
    float diff = 0.15 + 0.85 * max(dot(n, l), 0.0);
    float rim  = pow(1.0 - max(dot(n, -rd), 0.0), 3.0);

    // Glow only where rails catch the light.
    vec3 col = base * diff;
    col += railCol * rail * 1.15;
    col += railCol * rim * 0.22;

    // Fog dissolves the tunnel into a glowing core.
    float fog = 1.0 - exp(-0.030 * t * t);
    return mix(col, vec3(0.015, 0.005, 0.055), fog);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    uv = rot2(0.09 * sin(iTime * 0.37)) * uv;  // subtle camera roll

    vec3 ro = vec3(0.45 * sin(iTime * 0.33),
                   0.45 * cos(iTime * 0.27),
                  -iTime * 1.9);
    vec3 fw = normalize(vec3(0.10 * sin(iTime * 0.45),
                             0.07 * cos(iTime * 0.39), 1.0));
    vec3 rt = normalize(cross(vec3(0.0, 1.0, 0.0), fw));
    vec3 up = cross(fw, rt);
    vec3 rd = normalize(fw * 1.4 + rt * uv.x + up * uv.y);

    float t = 0.0;
    float halo = 0.0;
    bool hit = false;
    for (int i = 0; i < STEPS; i++){
        vec3 p = ro + rd * t;
        float d = map(p);
        // Only halo glow that is close to the wall and near the camera.
        halo += exp(-abs(d) * 26.0) * exp(-t * 0.22) * 0.020;
        t += max(d, 0.002);
        if (d < 0.002 * t + 0.002){ hit = true; break; }
        if (t > 40.0) break;
    }

    vec3 col = vec3(0.004, 0.004, 0.014);
    if (hit) col = shade(ro + rd * t, rd, t);
    col += vec3(0.20, 0.45, 1.00) * halo;
    col += vec3(0.55, 0.12, 0.85) * exp(-0.05 * t) * 0.30;

    col = col / (1.0 + col * 1.1);
    col = pow(col, vec3(0.4545));
    fragColor = vec4(col, 1.0);
}
