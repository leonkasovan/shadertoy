// Spark effect
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = fragCoord.xy/iResolution.xy;
    vec2 p = uv*2.0-1.0; p.x*=iResolution.x/iResolution.y;
    float t = iTime;
    vec3 col = vec3(0.02,0.02,0.05);
    for(int i=0;i<40;i++){
        float fi=float(i);
        float seed = hash(vec2(fi, floor(t*2.0+fi)));
        float age = fract(t*2.0 + fi*0.137);
        float ang = hash(vec2(fi*3.7, seed))*6.283;
        float sp = age*1.2;
        vec2 pos = vec2(cos(ang), sin(ang))*sp*0.8 + vec2(0, -age*0.3);
        float size = 0.012*(1.0-age);
        float d = length(p-pos);
        float spark = exp(-d/size);
        vec3 c = mix(vec3(1.0,0.9,0.4), vec3(1.0,0.3,0.0), age);
        col += c*spark*(1.0-age)*1.5;
    }
    // glow center
    col += vec3(1.0,0.6,0.2)*exp(-length(p)*4.0)*0.4;
    fragColor = vec4(col,1.0);
}
