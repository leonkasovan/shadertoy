// Paralax Cloud - puffy clouds on stacked parallax layers.
// Nearer layers: larger puffs, faster horizontal drift, sit lower.
// Farther layers: small puffs, slow drift, faded toward the sky.

float hash21(vec2 p){
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float vnoise(vec2 p){
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p){
    float v = 0.0, a = 0.55;
    mat2 r = mat2(0.8, -0.6, 0.6, 0.8);
    for(int i = 0; i < 4; i++){
        v += a * vnoise(p);
        p = r * p * 2.03;
        a *= 0.5;
    }
    return v;
}

// one cloud layer; d = 0 nearest ... 1 farthest (composited far first)
void layer(vec2 sc, float uvY, float t, float d, vec3 skyLow, inout vec3 col){
    float speed = mix(0.10, 0.012, d);   // slow drift; near/far ratio ~8x for parallax
    float scale = mix(0.7, 9.0, d);
    vec2 q = vec2(sc.x, uvY * 1.6) * scale;
    q.y += d * 57.0;              // decorrelate the layers
    q.x += t * speed * scale;     // parallax: near layers sweep past
    float n = fbm(q);
    n = mix(n, fbm(q + vec2(n * 1.2, -n * 0.9)), 0.65);  // fluffier warp
    float cover = smoothstep(mix(0.38, 0.46, d), mix(0.86, 0.74, d), n);
    float alpha = cover * mix(0.92, 0.45, d);
    float rho = smoothstep(0.10, 0.90, cover);
    vec3 cloud = mix(vec3(0.58, 0.66, 0.80), vec3(1.0, 1.0, 1.0), rho);
    cloud = mix(cloud, skyLow, 0.55 * d);   // aerial haze on far layers
    col = mix(col, cloud, alpha);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = fragCoord.xy / iResolution.xy;
    vec2 sc = vec2(uv.x * iResolution.x / iResolution.y, uv.y);

    vec3 skyTop = vec3(0.20, 0.46, 0.89);
    vec3 skyLow = vec3(0.72, 0.84, 0.97);
    vec3 col = mix(skyLow, skyTop, smoothstep(0.10, 1.0, uv.y));

    vec2 sunPos = vec2(0.78, 0.88);
    float sun = exp(-7.0 * length((uv - sunPos) * vec2(1.0, 1.7)));
    col += vec3(1.0, 0.97, 0.85) * sun * 0.45;

    for(int L = 4; L >= 1; L--){
        layer(sc, uv.y, iTime, float(L - 1) / 3.0, skyLow, col);
    }
    fragColor = vec4(col, 1.0);
}
