// Fractal flame
void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord.xy - 0.5*iResolution.xy)/iResolution.y;
    float t = iTime*0.4;
    vec2 z = uv*1.5;
    float iter=0.0;
    for(int i=0;i<60;i++){
        z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + vec2(sin(t)*0.3, cos(t*0.7)*0.3);
        if(dot(z,z)>4.0){ iter=float(i); break; }
    }
    float v = iter/60.0;
    vec3 col = mix(vec3(0.1,0.0,0.3), vec3(1.0,0.4,0.0), v);
    col = mix(col, vec3(1.0,0.9,0.5), pow(v,3.0));
    col = col * 1.6 + 0.15;
    fragColor = vec4(col,1.0);
}
