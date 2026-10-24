#define PI 3.1415926

vec3 palette(float hue) {
  vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
  vec3 p = abs(fract(hue + K.xyz) * 6.0 - K.www);
  return clamp(p - K.xxx, 0.0, 1.0);
}

float sd_segment( in vec2 p, in vec2 a, in vec2 b ) {
  vec2 pa = p-a, ba = b-a;
  float h = clamp( dot(pa,ba)/dot(ba,ba), 0.0, 1.0 );
  return length( pa - ba*h );
}

float sd_capsule(in vec2 p, in vec2 a, in vec2 b, in float r ) {
  return sd_segment(p, a, b) - r;
}

float fill_shape(in float d, in float blur) {
  return 1.0 - smoothstep(0.0 - blur, blur, d);
}

float capsule(in vec2 p, in vec2 a, in vec2 b, in float r, in float blur) {
    float d = sd_capsule(p, a, b, r);
    return fill_shape(d, blur);
}


void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;

    float t = fract(iTime);
    float blur = 2.0 / iResolution.y;
    float bar_radius = 0.06;
    vec2 bar_dir = vec2(-1.0, -2.0);
    float bar_speed = 9.0;
    vec2 bar_pos = t * bar_dir * bar_speed;

    vec3 color = vec3(0.0);

    for (float i = 0.0; i < 12.0; i++) {
        vec2 a = vec2(-1.0 + 0.15 * i, 1.0) - 5.1 * bar_dir - 0.25 * i * bar_dir;
        vec2 b = a + 5.0 * bar_dir;
        a += bar_pos;
        b += bar_pos;
        
        float bar_mask = capsule(uv - vec2(0.7, 0.0), a, b, bar_radius, blur);
        vec3 bar_color = palette(i / 12.0);

        color += bar_color * bar_mask;
    }

    fragColor = vec4(color, 1.0);
}
