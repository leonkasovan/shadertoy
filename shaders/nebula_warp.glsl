// Nebula Warp
// Drifting through a deep-space nebula. The gas is a domain-warped fbm with
// dark dust lanes punched through it; stars flow outward past the camera
// along the flight direction, streaking with speed.
#define PI 3.14159265359

float hash13(vec3 p){
    p = fract(p*0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x*p.y*p.z*(p.x + p.y + p.z));
}

float noise(vec3 x){
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f*f*(3.0 - 2.0*f);
    return mix(mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
                   mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
                   mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

float fbm(vec3 p){
    float s = 0.0, a = 0.5;
    for(int i = 0; i < 6; i++){
        s += a*noise(p);
        p = p*2.02 + vec3(11.3, 7.7, 3.1);
        a *= 0.5;
    }
    return s;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (2.0*fragCoord - iResolution.xy)/iResolution.y;
    float t = iTime*0.30;
    float r = length(uv);

    // Mild barrel distortion for a wide-angle feel.
    vec2 p = uv*(1.0 + 0.14*dot(uv, uv));

    vec3 col = vec3(0.004, 0.006, 0.016);

    // ---- Stars: radial flow, stretched along the direction of travel ----
    vec2 dir = normalize(p + 1e-5);
    for(int L = 0; L < 3; L++){
        float fl = float(L);
        float sc = 9.0 + fl*7.0;
        // Sample in a frame where the travel axis is vertical, then stretch.
        vec2 g = p*sc;
        g -= dir*(r*sc*0.85 + t*(3.0 + 2.0*fl));
        vec2 id = floor(g);
        vec2 f = fract(g) - 0.5;
        float h = hash13(vec3(id, fl*13.0 + 4.0));
        float len = 0.10 + 0.22*h;
        float star = smoothstep(0.050, 0.0, length(f*vec2(1.0, 1.0/len)));
        star *= step(0.915, h);
        float tw = 0.55 + 0.45*sin(iTime*2.5 + h*60.0);
        vec3 tint = mix(vec3(0.60, 0.75, 1.0), vec3(1.0, 0.80, 0.55), h);
        col += tint*star*tw*(1.6/(1.0 + fl*1.3));
    }

    // ---- Nebula gas: two warped fbm layers, dark dust carved out ----
    vec3 q = vec3(p*1.05, t*0.5);
    float w1 = fbm(q*0.7 + vec3(0.0, 0.0, t*0.11));
    vec3 wp = q + vec3(w1*1.9, w1*1.1, w1*0.8);
    float gas = fbm(wp*1.35);
    float dust = fbm(wp*2.6 + vec3(3.1, 9.4, t*0.3));
    gas = pow(clamp(gas*1.55 - 0.22, 0.0, 1.0), 2.0);
    // Dust lanes are where the second fbm dips — subtract their shadow.
    gas *= smoothstep(0.28, 0.58, dust);

    float hot = pow(clamp(gas*1.9, 0.0, 1.0), 2.0);
    vec3 neb = mix(vec3(0.02, 0.04, 0.13), vec3(0.02, 0.32, 0.46), smoothstep(0.02, 0.45, gas));
    neb = mix(neb, vec3(0.85, 0.20, 0.55), hot*0.9);
    neb += vec3(1.0, 0.62, 0.30)*pow(hot, 3.0)*0.8;
    col += neb*gas*3.4;

    // Distant star cluster / horizon glow.
    vec2 c = vec2(0.60*sin(t*0.21), 0.30*cos(t*0.17));
    col += vec3(0.50, 0.60, 1.0)*exp(-length(p - c)*6.0)*0.13;

    // Deep purple core along the flight axis.
    col += vec3(0.10, 0.05, 0.22)*exp(-r*r*2.0)*0.7;

    col *= 1.0 - 0.18*smoothstep(0.5, 1.7, r);
    col = pow(clamp(col, 0.0, 1.0), vec3(0.4545));
    fragColor = vec4(col, 1.0);
}
