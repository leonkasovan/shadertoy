// Mandelbulb bloom — power-8 escape-time fractal with orbit-trap colouring,
// rim light and distance-field ambient occlusion. A bounding sphere keeps
// the empty space cheap.

#define TAU 6.28318530718
#define STEPS 110
#define POWER 8.0
#define ITER 5
#define BOUND 1.45

mat2 rot2(float a){ float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }

float mandelbulb(vec3 p, out vec4 trap){
    vec3 z = p;
    float dr = 1.0;
    float r = length(z);
    trap = vec4(1e5);
    for (int i = 0; i < ITER; i++){
        r = max(length(z), 1e-9);
        if (r > 2.0) break;
        float theta = acos(clamp(z.z / r, -1.0, 1.0));
        float phi = atan(z.y, z.x);
        dr = pow(r, POWER - 1.0) * POWER * dr + 1.0;
        float zr = pow(r, POWER);
        theta *= POWER;
        phi *= POWER;
        z = zr * vec3(sin(theta) * cos(phi),
                      sin(theta) * sin(phi),
                      cos(theta)) + p;
        trap = min(trap, vec4(abs(z), r));
    }
    return 0.5 * log(max(r, 1e-6)) * r / dr;
}

vec3 calcNormal(vec3 p){
    vec4 tr;
    vec2 e = vec2(0.0008, 0.0);
    return normalize(vec3(
        mandelbulb(p + e.xyy, tr) - mandelbulb(p - e.xyy, tr),
        mandelbulb(p + e.yxy, tr) - mandelbulb(p - e.yxy, tr),
        mandelbulb(p + e.yyx, tr) - mandelbulb(p - e.yyx, tr)));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;

    float a = iTime * 0.18;
    vec3 ro = vec3(2.55 * sin(a), 1.05 + 0.35 * sin(iTime * 0.31), 2.55 * cos(a));
    vec3 fw = normalize(-ro);
    vec3 rt = normalize(cross(vec3(0.0, 1.0, 0.0), fw));
    vec3 up = cross(fw, rt);
    vec3 rd = normalize(fw * 1.85 + rt * uv.x + up * uv.y);

    // Ray vs bounding sphere around the origin.
    float b = dot(ro, rd);
    float c = dot(ro, ro) - BOUND * BOUND;
    float disc = b * b - c;
    float t0 = 0.0, t1 = -1.0;
    if (disc > 0.0){
        float sq = sqrt(disc);
        t0 = max(-b - sq, 0.0);
        t1 = -b + sq;
    }

    vec3 col = vec3(0.006, 0.008, 0.020);
    col += vec3(0.05, 0.02, 0.12) * exp(-length(uv - vec2(0.25, 0.15)) * 2.4);

    bool hit = false;
    float t = t0;

    if (t1 > 0.0){
        vec4 trap = vec4(0.0);
        for (int i = 0; i < STEPS; i++){
            float d = mandelbulb(ro + rd * t, trap);
            if (d < 0.0009 * t + 0.0004){ hit = true; break; }
            t += d * 0.9;
            if (t > t1) break;
        }
    }

    if (hit){
        vec3 p = ro + rd * t;
        vec3 n = calcNormal(p);
        vec4 tr;
        mandelbulb(p, tr);

        // Orbit trap -> deep blue/cyan/magenta ramp (no white blobs).
        float h = fract(tr.x * 1.3 + tr.y * 0.7 + iTime * 0.03);
        vec3 pal = mix(vec3(0.030, 0.120, 0.420),
                       vec3(0.050, 0.720, 0.900), smoothstep(0.00, 0.45, h));
        pal = mix(pal, vec3(0.900, 0.150, 0.550), smoothstep(0.55, 1.00, h));
        pal += vec3(1.00, 0.55, 0.15) * smoothstep(0.72, 1.0, tr.w) * 0.9;

        vec3 l1 = normalize(vec3(0.75, 0.85, 0.35));
        vec3 l2 = normalize(vec3(-0.65, -0.15, -0.70));
        float diff = max(dot(n, l1), 0.0);
        float back = max(dot(n, l2), 0.0);
        float rim = pow(1.0 - max(dot(n, -rd), 0.0), 3.0);

        // Cheap AO from the distance field.
        float occ = 0.0;
        for (int i = 1; i <= 4; i++){
            float hh = 0.015 * float(i * i);
            occ += (hh - mandelbulb(p + n * hh, tr)) / hh;
        }
        float ao = clamp(1.0 - 0.055 * occ, 0.0, 1.0);

        col = pal * (0.10 + 0.95 * diff) * ao;
        col += vec3(0.25, 0.55, 1.00) * rim * 0.35 * ao;
        col += vec3(0.95, 0.45, 0.20) * back * 0.10;
    } else {
        col += vec3(0.10, 0.22, 0.75) * exp(-t * 1.1) * 0.22;
    }

    // Soft bloom around the silhouette.
    col += vec3(0.10, 0.18, 0.55) * exp(-length(uv) * 2.2) * 0.06;

    col *= 1.0 - 0.40 * dot(uv, uv);
    col = col / (1.0 + col * 0.8);
    col = pow(col, vec3(0.4545));
    fragColor = vec4(col, 1.0);
}
