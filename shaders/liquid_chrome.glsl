// Liquid chrome — melted metal blobs floating in a synthetic sunset.
// Smooth-min SDF sphere cluster, analytic high-contrast studio environment,
// thin-film iridescence and a distance-field ambient occlusion.

#define STEPS 90

mat2 rot2(float a){ float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }

float smin(float a, float b, float k){
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// Five blobs orbiting on lissajous paths, fused into one liquid mass.
float map(vec3 p){
    float t = iTime * 0.7;
    float d = 1e9;
    for (int i = 0; i < 5; i++){
        float fi = float(i);
        float ang = t + fi * 1.2566;
        vec3 c = vec3(
            0.90 * sin(ang * (0.80 + 0.17 * fi)),
            0.52 * sin(ang * (1.10 + 0.09 * fi) + fi),
            0.90 * cos(ang * (0.93 + 0.13 * fi)));
        float rad = 0.58 + 0.12 * sin(t * 1.3 + fi * 2.1);
        d = smin(d, length(p - c) - rad, 0.42);
    }
    // Surface ripple so the metal looks molten.
    return d
         - 0.030 * sin(p.x * 6.0 + iTime) * sin(p.y * 6.0 - iTime)
         - 0.030 * sin(p.z * 6.0 + iTime * 0.7);
}

vec3 calcNormal(vec3 p){
    vec2 e = vec2(0.0015, 0.0);
    return normalize(vec3(
        map(p + e.xyy) - map(p - e.xyy),
        map(p + e.yxy) - map(p - e.yxy),
        map(p + e.yyx) - map(p - e.yyx)));
}

float ambientOcclusion(vec3 p, vec3 n){
    float occ = 0.0, sca = 1.0;
    for (int i = 1; i <= 4; i++){
        float h = 0.02 + 0.10 * float(i);
        occ += (h - map(p + n * h)) * sca;
        sca *= 0.70;
    }
    return clamp(1.0 - 1.6 * occ, 0.0, 1.0);
}

// Studio environment with hard light bars: that is what sells chrome.
vec3 env(vec3 d){
    float y = d.y;

    // Dark ground, bright sky, and a razor-thin horizon.
    vec3 e = mix(vec3(0.030, 0.035, 0.055), vec3(0.02, 0.02, 0.03),
                 smoothstep(-0.02, -0.5, y));
    e += vec3(0.85, 0.90, 1.00) * smoothstep(0.02, 0.18, y) * 0.55;

    // Big softbox strip high above.
    vec3 sd = normalize(vec3(0.35, 0.85, -0.40));
    e += vec3(1.0) * smoothstep(0.90, 0.97, dot(d, sd)) * 2.4;

    // Vertical neon bars on the horizon: classic chrome stretch.
    float az = atan(d.z, d.x);
    float bars = smoothstep(0.90, 0.99, abs(sin(az * 3.0 - 0.6)));
    e += vec3(0.30, 0.75, 1.00) * bars * smoothstep(0.25, -0.05, abs(y)) * 0.9;

    // Warm rim from below/behind.
    e += vec3(1.00, 0.35, 0.20) * smoothstep(0.55, 0.95, -d.y) * 0.25;

    // Sunset contact glow.
    e += vec3(1.00, 0.35, 0.55) * exp(-abs(y) * 16.0) * 0.55;
    return e;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;

    float ca = iTime * 0.22;
    vec3 ro = vec3(2.9 * sin(ca), 1.15 + 0.25 * sin(iTime * 0.4), 2.9 * cos(ca));
    vec3 ta = vec3(0.0, 0.05 * sin(iTime * 0.5), 0.0);
    vec3 fw = normalize(ta - ro);
    vec3 rt = normalize(cross(vec3(0.0, 1.0, 0.0), fw));
    vec3 up = cross(fw, rt);
    vec3 rd = normalize(uv.x * rt + uv.y * up + 1.7 * fw);

    float t = 0.0;
    bool hit = false;
    for (int i = 0; i < STEPS; i++){
        float d = map(ro + rd * t);
        if (d < 0.0012){ hit = true; break; }
        t += d * 0.75;
        if (t > 12.0) break;
    }

    vec3 col = env(rd) * 0.30;                 // dim background

    if (hit){
        vec3 p = ro + rd * t;
        vec3 n = calcNormal(p);
        vec3 r = reflect(rd, n);
        vec3 e = env(r);
        float fres = pow(1.0 - clamp(dot(n, -rd), 0.0, 1.0), 5.0);

        // Iridescent tint, subtle: chrome first, colour second.
        vec3 irid = 0.5 + 0.5 * cos(5.0 * n + vec3(0.0, 2.1, 4.2) + iTime * 0.4);
        vec3 metal = mix(vec3(0.86, 0.88, 0.92), vec3(0.55, 0.72, 0.95), 0.35);
        metal *= 0.88 + 0.12 * irid;

        // Fake sharp key highlight (sun/spec).
        vec3 ld = normalize(vec3(0.35, 0.85, -0.40));
        float spec = pow(max(dot(r, ld), 0.0), 350.0) * 5.0;

        float occ = ambientOcclusion(p, n);
        col = e * metal * (0.35 + 0.65 * occ);
        col += vec3(1.0, 0.97, 0.92) * spec;
        col += vec3(1.0, 0.45, 0.65) * pow(max(dot(r, normalize(vec3(-0.2, 0.1, 0.9))), 0.0), 18.0) * 0.35;
        col = mix(col, vec3(0.01, 0.012, 0.02), 0.20 * (1.0 - fres));
    }

    // Ground shadow / reflection suggestion underneath the blobs.
    float glow = exp(-length(uv - vec2(0.0, -0.55)) * 3.0);
    col += vec3(1.0, 0.25, 0.45) * glow * 0.02;

    col *= 1.0 - 0.30 * dot(uv, uv);
    col = col / (1.0 + col * 0.9);
    col = pow(col, vec3(0.4545));
    fragColor = vec4(col, 1.0);
}
