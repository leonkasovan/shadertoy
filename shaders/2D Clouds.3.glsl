/*
    Fast White Clouds
    Optimized for Mali-G31 / R36S

    Features:
    - Fast value noise
    - 3 octave FBM
    - Dense white clouds
    - Low GPU cost
*/

const float CLOUD_SCALE = 1.4;
const float CLOUD_SPEED = 0.025;

const vec3 SKY_HORIZON = vec3(0.55, 0.75, 1.00);
const vec3 SKY_ZENITH  = vec3(0.20, 0.45, 0.85);

const mat2 M = mat2(
     1.6,  1.2,
    -1.2,  1.6
);

//////////////////////////////////////////////////////////
// Fast hash
//////////////////////////////////////////////////////////

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

//////////////////////////////////////////////////////////
// Fast value noise
//////////////////////////////////////////////////////////

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

//////////////////////////////////////////////////////////
// 3 octave FBM
//////////////////////////////////////////////////////////

float fbm(vec2 p)
{
    float v = 0.0;

    v += noiseFast(p) * 0.50;
    p = M * p;

    v += noiseFast(p) * 0.25;
    p = M * p;

    v += noiseFast(p) * 0.125;

    return v;
}

//////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord.xy / iResolution.xy;

    vec2 p = uv;
    p.x *= iResolution.x / iResolution.y;

    float t = iTime * CLOUD_SPEED;

    //////////////////////////////////////////////////////
    // Base cloud layer
    //////////////////////////////////////////////////////

    float n1 = fbm(
        p * CLOUD_SCALE +
        vec2(t, 0.0)
    );

    //////////////////////////////////////////////////////
    // Detail cloud layer
    //////////////////////////////////////////////////////

    float n2 = fbm(
        p * CLOUD_SCALE * 2.5 -
        vec2(t * 1.4, 0.0)
    );

    //////////////////////////////////////////////////////
    // Dense cloud field
    //////////////////////////////////////////////////////

    float clouds =
        n1 * 0.8 +
        n2 * 0.2;

    clouds = smoothstep(
        0.28,   // lower = more cloud
        0.60,
        clouds
    );

    //////////////////////////////////////////////////////
    // Bright white clouds
    //////////////////////////////////////////////////////

    float highlight =
        smoothstep(
            0.45,
            0.90,
            n1 + n2 * 0.5
        );

    //////////////////////////////////////////////////////
    // Sky gradient
    //////////////////////////////////////////////////////

    vec3 sky = mix(
        SKY_HORIZON,
        SKY_ZENITH,
        uv.y
    );

    //////////////////////////////////////////////////////
    // White cloud color
    //////////////////////////////////////////////////////

    vec3 cloudColor = mix(
        vec3(0.90, 0.92, 0.95),
        vec3(1.00, 1.00, 1.00),
        highlight
    );

    //////////////////////////////////////////////////////
    // Blend
    //////////////////////////////////////////////////////

    vec3 color = mix(
        sky,
        cloudColor,
        clouds
    );

    fragColor = vec4(color, 1.0);
}