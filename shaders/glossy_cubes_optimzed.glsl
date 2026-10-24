// Optimized from https://www.shadertoy.com/view/4sSBW3
// Created by https://www.shadertoy.com/user/Klems
//
// Optimization summary (target: 60 FPS):
//  - The distance field checks 5 cubes (centre + 4 orthogonal neighbours)
//    instead of 9 (the 4 diagonal corner cubes contribute least and are
//    omitted). This cheapens the march, surface, normal and reflection.
//  - The glossy cone-trace uses the same animated field (so reflections match
//    the animated cubes) but with fewer tracing steps.
//  - iTime is hoisted into a local.

#define PI 3.14159265359

vec3 hsv( in vec3 c ) {
    vec3 rgb = clamp(abs(mod(c.x*6.0+vec3(0,4,2),6.0)-3.0)-1.0, 0.0, 1.0);
	return c.z * mix(vec3(1), rgb, c.y);
}

float sdBox( in vec3 p, in vec3 b ) {
    vec3 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0);
}

vec2 hash22( in vec2 p ) {
	vec3 p3 = fract(vec3(p.xyx) * vec3(.1031, .1030, .0973));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float perlin( in float x, in float seed ) {
    float a = floor(x);
    float b = a + 1.0;
    float f = x - a;
    f = f*f*(3.0-2.0*f);
    return mix(hash22(vec2(seed, a)).x, hash22(vec2(seed, b)).x, f);
}

// distance function (animated heights, 5-cube neighbourhood)
float de( in vec3 p, in float r, in float time, out vec3 color ) {
    float d = -p.z;
    vec2 center = floor(p.xy)+0.5;
    color = vec3(0);
    float colorAcc = 0.0;
    const vec2 dirs[5] = vec2[5](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0),
        vec2(0.0, 1.0), vec2(0.0, -1.0));
    for (int i = 0 ; i < 5 ; i++) {
        vec2 offset = center + dirs[i];
        vec3 inCenter = p - vec3(offset, 0);
        vec2 rnd = hash22(offset);
        float height = 1.0 + perlin( time+rnd.x, rnd.y )*0.75;
        vec3 colorHere = hsv( vec3(-offset.y*0.1 + rnd.y*0.3, 1, 1) );
        float dist = sdBox(inCenter, vec3(vec2(0.45), height))-0.05;
        d = min(d, dist);
        float alpha = max(0.001, smoothstep(r, -r, dist));
        color += colorHere*alpha;
        colorAcc += alpha;
    }
    color /= colorAcc;
    return d;
}

// normal function - reuses the surface distance 'here' as the center tap
vec3 normal( in vec3 p, float here, in float time ) {
	vec3 e = vec3(0.0, 0.001, 0.0);
    vec3 dummy = vec3(0);
	return normalize(vec3(
		here-de(p-e.yxx, 0.0, time, dummy),
		here-de(p-e.xyx, 0.0, time, dummy),
		here-de(p-e.xxy, 0.0, time, dummy)));
}

// cone-tracing
vec4 trace( in vec3 from, in vec3 dir, in float sinTheta, in float time ) {
    float totdist = 0.01;
    vec4 acc = vec4(0, 0, 0, 1);
    for (int i = 0 ; i < 3 ; i++) {
        vec3 p = from + totdist * dir;
        float r = totdist*sinTheta;
        vec3 color = vec3(0);
        float dist = de(p, r, time, color);
        float alpha = clamp(dist / r * -0.5 + 0.5, 0.0, 1.0);
        acc.rgb += acc.a * alpha * color;
        acc.a *= 1.0 - alpha;
        if (acc.a < 0.01) break;
        totdist += abs(dist);
    }
    acc.a = 1.0 - acc.a;
    return acc;
}

void mainImage( out vec4 fragColor, in vec2 fragCoord ) {
    float time = iTime;
    vec2 uv = fragCoord.xy / iResolution.xy * 2.0 - 1.0;
	uv.y *= iResolution.y / iResolution.x;

	vec3 from = vec3(time, 0.2*time, -5)+vec3(iMouse.xy*0.04, 0);
	vec3 dir = normalize(vec3(uv, 0.5));

	float totdist = 0.0;
	for (int steps = 0 ; steps < 14 ; steps++) {
		vec3 p = from + totdist * dir;
        vec3 dummy = vec3(0);
		float dist = de(p, 0.0, time, dummy);
		totdist += dist;
		if (dist < 0.001) break;
	}

    vec3 p = from + totdist * dir;
    vec3 color = vec3(0);
    float sd = de(p, 0.01, time, color);
	vec3 norm = normal(p, sd, time);

    vec3 refl = reflect(dir, norm);
    vec4 gloss = trace(p, refl, 0.5, time);
    gloss.rgb = mix(vec3(1), gloss.rgb, gloss.a);

    float fres = pow(max(0.0, dot(-dir, norm)), 2.0);
    fragColor.rgb = mix(color, gloss.rgb, fres);
    fragColor.a = 1.0;
}