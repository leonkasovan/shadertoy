// Kaleido Bloom
// A 10-fold kaleidoscopic fractal that folds space and lights it with a
// cosine palette. Each fold leaves a glowing filament behind.
#define PI 3.14159265359

mat2 rot(float a){
    float c = cos(a), s = sin(a);
    return mat2(c, -s, s, c);
}

vec3 pal(float t){
    return 0.5 + 0.5*cos(6.28318*(vec3(0.00, 0.35, 0.68) + t));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (2.0*fragCoord - iResolution.xy)/iResolution.y;
    float t = iTime*0.35;

    // Kaleidoscope: fold the polar angle into 10 symmetric wedges.
    float r = length(uv);
    float a = atan(uv.y, uv.x) + t*0.4;
    float seg = 2.0*PI/10.0;
    a = abs(mod(a, seg) - 0.5*seg);
    vec2 p = r*vec2(cos(a), sin(a));

    // Fractal filaments: repeatedly fold, rotate and shrink the space.
    float glow = 0.0;
    float hue = 0.0;
    float wsum = 0.0;
    vec2 q = p;
    for(int i = 0; i < 7; i++){
        float fi = float(i);
        q = abs(q) - vec2(0.34, 0.28 + 0.02*sin(t*1.7 + fi));
        q *= rot(0.55 + 0.25*sin(t + fi*1.3));
        float d = length(q) - (0.075 - 0.0035*fi);
        float w = 0.010/(0.010 + abs(d));
        glow += w;
        // Each fold scatters a slightly different hue, weighted by its glow.
        hue += w*(0.11*fi + 0.22*atan(q.y, q.x)/PI + 0.05*sin(t*0.7 + fi));
        wsum += w;
        q *= 1.62;
    }
    hue /= max(wsum, 1e-4);

    // Sharp filaments on top of a soft inner haze.
    float fil = pow(clamp(glow*0.42, 0.0, 1.6), 2.1);
    vec3 col = pal(0.42 + hue + 0.10*t + 0.16*p.x)*fil*1.45;
    col += pal(0.62 + 0.12*sin(t*0.9))*(exp(-r*3.4)*0.10 + exp(-r*11.0)*0.30);

    // Icy highlights where the folds pile up.
    col += vec3(0.75, 0.92, 1.0)*pow(clamp(glow - 2.6, 0.0, 1.4), 1.6)*0.20;

    // Soft vignette + gentle tone curve.
    col *= 1.0 - 0.12*dot(uv, uv);
    col = pow(clamp(col, 0.0, 1.0), vec3(0.4545));
    fragColor = vec4(col, 1.0);
}
