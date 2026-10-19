// Ultra-cheap clouds — Mali-G31 target 30 FPS
// ~6 noise evals per pixel, softer cloud tones

const float speed = 0.03;
const vec3 skycolour1 = vec3(0.3, 0.5, 0.7);
const vec3 skycolour2 = vec3(0.5, 0.75, 1.0);
const mat2 m = mat2( 1.6,  1.2, -1.2,  1.6 );

vec2 hash( vec2 p ) {
	p = vec2(dot(p,vec2(127.1,311.7)), dot(p,vec2(269.5,183.3)));
	return -1.0 + 2.0*fract(sin(p)*43758.5453123);
}

float noise( in vec2 p ) {
    const float K1 = 0.366025404;
    const float K2 = 0.211324865;
	vec2 i = floor(p + (p.x+p.y)*K1);
    vec2 a = p - i + (i.x+i.y)*K2;
    vec2 o = (a.x>a.y) ? vec2(1.0,0.0) : vec2(0.0,1.0);
    vec2 b = a - o + K2;
	vec2 c = a - 1.0 + 2.0*K2;
    vec3 h = max(0.5-vec3(dot(a,a), dot(b,b), dot(c,c) ), 0.0 );
	vec3 n = h*h*h*h*vec3( dot(a,hash(i+0.0)), dot(b,hash(i+o)), dot(c,hash(i+1.0)));
    return dot(n, vec3(70.0));
}

void mainImage( out vec4 fragColor, in vec2 fragCoord ) {
    vec2 p = fragCoord.xy / iResolution.xy;
	vec2 uv = p*vec2(iResolution.x/iResolution.y, 1.0);
    float time = iTime * speed;

    // 3-iter FBM for base shape
    vec2 q = uv * 0.55;
    float f = noise(q); q = m*q;
    f += 0.5*noise(q); q = m*q;
    f += 0.25*noise(q);

    // 3-iter ridged detail
    vec2 ru = uv * 1.1 - vec2(f - time, f - time * 0.8);
    float r = abs(noise(ru)); ru = m*ru + time;
    r += 0.65*abs(noise(ru)); ru = m*ru + time;
    r += 0.42*abs(noise(ru));

    f = clamp(0.2 + 6.0 * f * r, 0.0, 1.0);

    vec3 sky = mix(skycolour2, skycolour1, p.y);
    // grey-blue clouds, not white
    vec3 cloud = vec3(0.85, 0.88, 0.93) * clamp(0.4 + 0.4*r, 0.0, 1.0);

    vec3 result = mix(sky, cloud, f);

	fragColor = vec4( result, 1.0 );
}
