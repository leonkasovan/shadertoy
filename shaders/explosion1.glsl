// Realistic-ish random explosions
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
float noise(vec2 p){
    vec2 i=floor(p), f=fract(p);
    f=f*f*(3.0-2.0*f);
    float a=hash(i), b=hash(i+vec2(1,0)), c=hash(i+vec2(0,1)), d=hash(i+vec2(1,1));
    return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}
float fbm(vec2 p){ float v=0.,a=.5; for(int i=0;i<5;i++){v+=a*noise(p); p*=2.1; a*=.5;} return v; }

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = fragCoord.xy / iResolution.xy;
    vec2 p = uv*2.0-1.0; p.x *= iResolution.x/iResolution.y;
    vec3 col = mix(vec3(0.02,0.03,0.08), vec3(0.05,0.08,0.15), uv.y);

    float t = iTime;
    for(int i=0;i<7;i++){
        float fi=float(i);
        float cycle = floor(t*0.4 + fi*0.73);
        float seed = hash(vec2(fi, cycle));
        vec2 center = vec2((hash(vec2(fi*3.1,cycle))-0.5)*1.4,
                           (hash(vec2(fi*7.2,cycle))-0.3)*0.7);
        float age = fract(t*0.4 + fi*0.73);
        float size = 0.6 + hash(vec2(fi*11.3, cycle))*1.2;
        // ease
        float e = smoothstep(0.0,0.08,age)*(1.0-smoothstep(0.25,1.0,age));
        float radius = (0.05 + pow(age,0.45)*0.9) * size;
        float d = length(p-center);
        // fireball
        float n = fbm((p-center)*6.0 + age*3.0);
        float ball = smoothstep(radius, radius*0.2, d + n*0.15);
        // core
        float core = exp(-d*d/(0.01+age*0.02));
        // smoke
        float smokeR = radius*1.6;
        float smoke = smoothstep(smokeR, smokeR*0.5, d + n*0.25) * (1.0-ball);
        // color
        vec3 fireCol = mix(vec3(1.0,0.15,0.0), vec3(1.0,0.9,0.3), core);
        fireCol = mix(fireCol, vec3(1.0,0.5,0.1), ball*0.5);
        vec3 smokeCol = vec3(0.15,0.15,0.18);
        col += fireCol * ball * e * 1.4;
        col += fireCol * core * e * 0.8;
        col += smokeCol * smoke * e * 0.6;
        // sparks
        for(int k=0;k<6;k++){
            float fk=float(k);
            float ang = hash(vec2(fk,seed))*6.28;
            float sp = age*1.2 + fk*0.05;
            vec2 spPos = center + vec2(cos(ang),sin(ang))*sp*0.8;
            float spSize = 0.015*(1.0-age);
            float spark = exp(-length(p-spPos)/spSize);
            col += vec3(1.0,0.7,0.2)*spark*e*0.6;
        }
    }
    // vignette
    col *= 1.0 - 0.3*length(uv-0.5);
    fragColor = vec4(col,1.0);
}
