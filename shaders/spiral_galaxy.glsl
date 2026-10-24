// Spiral galaxy — log-spiral arms, turbulent nebulae, dust lanes and
// three rotating star layers. Fully analytic (no textures, no buffers).

#define TAU 6.28318530718

mat2 rot2(float a){ float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }

float hash21(vec2 p){
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float noise(vec2 p){
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p){
    float s = 0.0, amp = 0.5;
    mat2 R = rot2(0.6);
    for (int i = 0; i < 5; i++){
        s += amp * noise(p);
        p = R * p * 2.03 + 7.1;
        amp *= 0.5;
    }
    return s;
}

// One layer of point stars on a jittered grid.
float starLayer(vec2 p, float scale, float salt, float sparkle){
    p *= scale;
    vec2 g = floor(p), f = fract(p);
    vec2 o = vec2(hash21(g + salt), hash21(g + salt + 41.7));
    float d = length(f - o);
    float h = hash21(g * 1.37 + salt * 3.11);
    float bright = pow(h, 9.0 + sparkle * 7.0);
    float twinkle = 0.72 + 0.28 * sin(iTime * (2.0 + 6.0 * h) + h * 40.0);
    return bright * twinkle * exp(-d * d * 220.0);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime * 0.12;

    // Tilt and rotate the disk.
    vec2 p = rot2(-0.42) * uv;
    vec2 q = vec2(p.x, p.y / 0.55);            // squash => inclined disk
    q = rot2(t * 0.6) * q;

    float r = length(q);
    float a = atan(q.y, q.x);
    float phase = a * 2.0 - log(r + 1e-3) * 3.2 + t * 2.4;   // log-spiral

    vec2 wq = rot2(t) * q;
    float neb = fbm(wq * 3.0 + vec2(0.0, t * 0.7));
    float clump = 0.55 + 0.75 * neb;

    float arm = pow(0.5 + 0.5 * sin(phase), 3.0);
    float disk = exp(-r * 2.6);
    float dens = disk * (0.28 + 1.10 * arm) * clump;

    // Hot core -> violet arms -> cold outskirts.
    vec3 coreCol = vec3(1.00, 0.78, 0.42);
    vec3 armCol  = vec3(0.22, 0.48, 1.00);
    vec3 outCol  = vec3(0.72, 0.16, 0.90);
    vec3 col = coreCol * exp(-r * 6.5) * 1.15;
    col += mix(armCol, outCol, smoothstep(0.10, 1.00, r)) * dens * 1.25;

    // Dust filaments hugging the arms.
    float dust = fbm(wq * 7.0 - vec2(t * 0.5));
    float dustMask = smoothstep(0.55, 0.85, dust) * (1.0 - smoothstep(0.2, 1.4, r));
    col *= 1.0 - 0.70 * dustMask;

    // Deep space background.
    col += vec3(0.006, 0.010, 0.030);
    col += vec3(0.018, 0.012, 0.055) * fbm(wq * 1.6);

    // Stars live in disk coordinates, so they rotate with the galaxy.
    float st = 0.0;
    st += starLayer(q, 22.0, 1.0, 0.0) * 1.00;
    st += starLayer(q, 46.0, 9.0, 1.0) * 0.55;
    st += starLayer(q, 90.0, 23.0, 1.0) * 0.30;
    st *= (1.0 - smoothstep(0.3, 2.2, r)) * 0.95 + 0.05;
    col += vec3(0.80, 0.88, 1.00) * st * 1.25;

    // A few warm foreground stars.
    col += vec3(1.0, 0.85, 0.70) * starLayer(uv * 1.1, 30.0, 57.0, 1.0) * 0.8;

    col *= 1.0 - 0.55 * dot(uv, uv);           // vignette
    col = col / (1.0 + col * 0.8);
    col = pow(col, vec3(0.4545));
    fragColor = vec4(col, 1.0);
}
