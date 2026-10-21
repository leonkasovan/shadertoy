// Plasma waves
void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = fragCoord.xy/iResolution.xy;
    float t = iTime;
    float v = sin(uv.x*10.0 + t) + sin(uv.y*12.0 - t*1.3)
            + sin((uv.x+uv.y)*8.0 + t*0.7) + sin(sqrt(uv.x*uv.x+uv.y*uv.y)*15.0 - t*2.0);
    v = (v+4.0)/8.0;
    vec3 col = 0.5 + 0.5*cos(6.2831*(v + vec3(0.0,0.33,0.67)));
    fragColor = vec4(col,1.0);
}
