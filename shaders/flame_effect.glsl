// Flame effect (natural)
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float noise(vec2 p){ vec2 i=floor(p),f=fract(p); f=f*f*(3.0-2.0*f);
 float a=hash(i),b=hash(i+vec2(1,0)),c=hash(i+vec2(0,1)),d=hash(i+vec2(1,1));
 return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);}
float fbm(vec2 p){ float v=0.,a=.5; for(int i=0;i<5;i++){v+=a*noise(p); p*=2.0; a*=.5;} return v;}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = fragCoord.xy/iResolution.xy;
    vec2 p = uv*2.0-1.0; p.x*=iResolution.x/iResolution.y;
    float t = iTime;

    // flame center with slight sway
    float sway = sin(t*1.2)*0.06 + sin(t*2.7)*0.03;
    float x = p.x - sway*(1.0 - (p.y+0.3));
    float y = p.y + 0.35;

    // vertical flame shape narrowing upward
    float width = 0.25*(1.0 - y*0.9);
    float shape = 1.0 - smoothstep(0.0, width, abs(x));

    // turbulence along flame
    float n = fbm(vec2(x*6.0, y*4.0 - t*3.0));
    float flame = shape * (0.55 + 0.45*n);
    flame *= smoothstep(1.0, 0.0, y); // fade top
    flame = clamp(flame, 0.0, 1.0);

    // color gradient
    float f = y + n*0.15;
    vec3 col = mix(vec3(1.0,0.1,0.0), vec3(1.0,0.5,0.0), f);
    col = mix(col, vec3(1.0,0.9,0.4), pow(f,2.0));
    col *= flame;

    // glow
    col += vec3(1.0,0.35,0.0)*exp(-abs(x)*4.0)*0.25*(1.0-y);

    fragColor = vec4(col,1.0);
}
