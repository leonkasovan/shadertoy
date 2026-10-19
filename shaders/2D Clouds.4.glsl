/*
    Fast Clouds for Mali-G31 / R36S

    Features:
    - Fast value noise
    - 3-octave FBM
    - No simplex noise
    - No expensive ridged-noise loops
    - Suitable for low-end ARM GPUs
*/

const float CLOUD_SCALE = 1.2;
const float CLOUD_SPEED = 0.03;

const vec3 SKY_BOTTOM = vec3(0.2, 0.4, 0.6);
const vec3 SKY_TOP    = vec3(0.4, 0.7, 1.0);

const mat2 M = mat2(
     1.6,  1.2,
    -1.2,  1.6
);

////////////////////////////////////////////////////////////
// Fast hash
////////////////////////////////////////////////////////////

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

////////////////////////////////////////////////////////////
// Fast value noise
////////////////////////////////////////////////////////////

float noiseFast(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);

    vec2 u = f * f * (3.0 - 2.0 * f);

    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));

    return mix(
        mix(a, b, u.x),
        mix(c, d, u.x),
        u.y
    );
}

////////////////////////////////////////////////////////////
// 3-octave FBM
////////////////////////////////////////////////////////////

float fbm(vec2 p)
{
    float sum = 0.0;

    sum += 0.500 * noiseFast(p);
    p = M * p;

    sum += 0.250 * noiseFast(p);
    p = M * p;

    sum += 0.125 * noiseFast(p);

    return sum;
}

////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 p = fragCoord.xy / iResolution.xy;

    vec2 uv = p;
    uv.x *= iResolution.x / iResolution.y;

    float t = iTime * CLOUD_SPEED;

    ////////////////////////////////////////////////////////
    // Large cloud shapes
    ////////////////////////////////////////////////////////

    float large =
        fbm(uv * CLOUD_SCALE + vec2(t, 0.0));

    ////////////////////////////////////////////////////////
    // Detail layer
    ////////////////////////////////////////////////////////

    float detail =
        fbm(uv * CLOUD_SCALE * 2.5 - vec2(t * 1.5, 0.0));

    ////////////////////////////////////////////////////////
    // Combine
    ////////////////////////////////////////////////////////

    float clouds =
        large * 0.75 +
        detail * 0.25;

    clouds = smoothstep(
        0.38,
        0.68,
        clouds
    );

    ////////////////////////////////////////////////////////
    // Soft cloud edges
    ////////////////////////////////////////////////////////

    float softness =
        smoothstep(0.0, 1.0, large);

    clouds *= softness;

    ////////////////////////////////////////////////////////
    // Sky gradient
    ////////////////////////////////////////////////////////

    vec3 sky =
        mix(
            SKY_TOP,
            SKY_BOTTOM,
            p.y
        );

    ////////////////////////////////////////////////////////
    // Simple cloud lighting
    ////////////////////////////////////////////////////////

    vec3 cloudCol =
        mix(
            vec3(0.75, 0.75, 0.78),
            vec3(1.0, 1.0, 0.98),
            detail
        );

    vec3 color =
        mix(
            sky,
            cloudCol,
            clouds
        );

    fragColor = vec4(color, 1.0);
}