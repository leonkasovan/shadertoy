// Neon tunnel
void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord.xy - 0.5*iResolution.xy)/iResolution.y;
    float t = iTime;
    float r = length(uv);
    float a = atan(uv.y, uv.x);
    float z = 1.0/(r+0.001);
    vec2 p = vec2(a*3.0 + t, z + t*2.0);
    float v = 0.0;
    for(int i=0;i<6;i++){ v += sin(p.x*float(i+1)+p.y*float(i+2)+t*0.5)/float(i+1); }
    v = (v+1.0)*0.5;
    vec3 col = mix(vec3(0.05,0.0,0.2), vec3(0.0,0.8,1.0), v);
    col += vec3(1.0,0.3,0.8)*pow(v,4.0);
    col *= 1.0 - smoothstep(0.0,0.6,r);
    col += vec3(0.2,0.0,0.4)*exp(-r*3.0);
    fragColor = vec4(col,1.0);
}
