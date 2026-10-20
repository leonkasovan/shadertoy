// ripple.glsl - raindrop rings on dark water.
// Rings expand, fade with distance and age. Hold the mouse to stir.

float hash21(vec2 p){
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

vec2 asp(vec2 frag){
    return (frag - 0.5 * iResolution.xy) / iResolution.y + 0.5;
}

const float SPD  = 2.2;   // ring expansion speed
const float FREQ = 16.0;  // ripple wavelength density
const float MAX_AGE = 2.4;  // droplet lifetime

// one expanding ring born 'age' seconds ago at 'src'
float ring(vec2 p, vec2 src, float age){
    if (age <= 0.0) return 0.0;
    float r = length(p - src) * 8.0;
    float front = r - age * SPD;
    if (front > 0.0) return 0.0;      // wavefront has not arrived yet
    float h = sin(front * FREQ)
         * exp(front * 1.4)           // decay behind the front
         * exp(-r * 0.30)             // with distance from the drop
         * exp(-age * 0.7);           // and with age
    h *= smoothstep(0.0, 0.3, -front);                  // soft leading edge
    h *= 1.0 - smoothstep(MAX_AGE * 0.88, MAX_AGE, age); // dissolve over the final 12% of life
    return h;
}

// total water height at p, time t
float field(vec2 p, float t){
    float h = 0.0;
    // five staggered raindrops from scattered spots
    for(int i = 0; i < 5; i++){
        float fi = float(i);
        vec2 src = vec2(0.18, 0.14) + vec2(0.64, 0.68) *
                   hash21(vec2(fi * 3.71 + 1.3, fi * 1.77 + 7.2));
        float age = fract(t * 0.45 + fi * 0.31) * MAX_AGE;
        h += ring(p, src, age);
    }
    // stir with the mouse while pressed
    if (iMouse.z > 0.0){
        vec2 m = asp(iMouse.xy);
        h += ring(p, m, fract(t * 0.8) * 1.8);
    }
    return h;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 p = asp(fragCoord);
    float t = iTime;

    // water normal from the height field (finite differences)
    float e = 0.004;
    float h  = field(p, t);
    float dhx = field(p + vec2(e, 0.0), t) - h;
    float dhy = field(p + vec2(0.0, e), t) - h;
    vec3 n = normalize(vec3(-dhx * 90.0, -dhy * 90.0, 1.0));

    // base water, deeper toward the top
    vec3 col = mix(vec3(0.02, 0.10, 0.28), vec3(0.05, 0.20, 0.42), p.y);

    // diffuse from a low sun
    vec3 lightDir = normalize(vec3(-0.4, 0.7, 0.6));
    float diff = clamp(dot(n, lightDir), 0.0, 1.0);
    col *= 0.55 + 0.75 * diff;

    // specular glints on the ring slopes
    vec3 hlf = normalize(lightDir + vec3(0.0, 0.0, 1.0));
    float spec = pow(clamp(dot(n, hlf), 0.0, 1.0), 60.0);
    col += vec3(0.75, 0.92, 1.0) * spec;

    // cyan tint on crests
    col += vec3(0.30, 0.62, 0.90) * max(h, 0.0) * 0.5;

    // vignette
    col *= 1.0 - 0.35 * length(p - 0.5);
    fragColor = vec4(col, 1.0);
}
