// Aurora borealis
void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = fragCoord.xy/iResolution.xy;
    vec2 p = uv*2.0-1.0; p.x*=iResolution.x/iResolution.y;
    float t = iTime*0.3;
    float y = p.y;
    vec3 col = mix(vec3(0.02,0.05,0.15), vec3(0.0,0.0,0.08), uv.y);
    for(int i=0;i<4;i++){
        float fi=float(i);
        float wave = sin(p.x*2.0+fi*1.3 + t*(1.0+fi*0.3))*0.15 + sin(p.x*3.5 - t*0.7)*0.08;
        float band = exp(-abs(y - (0.2+fi*0.15 + wave))*8.0);
        vec3 aurora = mix(vec3(0.1,0.9,0.4), vec3(0.6,0.2,0.9), fi/4.0);
        col += aurora*band*(0.6+0.4*sin(t+fi));
    }
    // stars
    float star = step(0.995, fract(sin(dot(uv,vec2(12.9898,78.233)))*43758.545));
    col += vec3(1.0)*star*0.8;
    fragColor = vec4(col,1.0);
}
