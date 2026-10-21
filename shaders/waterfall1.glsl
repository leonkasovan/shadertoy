// Waterfall scene (improved)
float hash21(vec2 p){ p=fract(p*vec2(123.34,456.21)); p+=dot(p,p+45.32); return fract(p.x*p.y);}
float vnoise(vec2 p){ vec2 i=floor(p),f=fract(p); f=f*f*(3.0-2.0*f);
 float a=hash21(i),b=hash21(i+vec2(1,0)),c=hash21(i+vec2(0,1)),d=hash21(i+vec2(1,1));
 return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);}
float fbm(vec2 p){ float v=0.,a=.55; mat2 r=mat2(.8,-.6,.6,.8);
 for(int i=0;i<4;i++){v+=a*vnoise(p); p=r*p*2.03; a*=.5;} return v;}

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = fragCoord.xy / iResolution.xy;
    vec2 sc = vec2(uv.x*iResolution.x/iResolution.y, uv.y);
    float t = iTime;

    // sky
    vec3 skyTop = vec3(0.20,0.46,0.89);
    vec3 skyLow = vec3(0.72,0.84,0.97);
    vec3 col = mix(skyLow, skyTop, smoothstep(0.10,1.0,uv.y));

    // slow clouds right->left (parallax style)
    for(int L=3; L>=1; L--){
        float d = float(L-1)/2.0;
        float speed = mix(0.08,0.01,d);
        float scale = mix(1.0,4.0,d);
        vec2 q = vec2(sc.x, uv.y*1.6)*scale;
        q.y += d*57.0;
        q.x -= t*speed*scale; // right to left
        float n = fbm(q);
        float cover = smoothstep(mix(0.38,0.46,d), mix(0.86,0.74,d), n);
        float alpha = cover*mix(0.92,0.45,d);
        vec3 cloud = mix(vec3(0.9,0.95,1.0), vec3(1.0), 0.6);
        col = mix(col, cloud, alpha*0.8);
    }

    // sun
    vec2 sunPos = vec2(0.78,0.88);
    float sun = exp(-7.0*length((uv-sunPos)*vec2(1.0,1.7)));
    col += vec3(1.0,0.97,0.85)*sun*0.45;

    // highland
    float hill = 0.55 + 0.08*sin(uv.x*6.0) + 0.05*fbm(vec2(uv.x*8.0,1.0));
    vec3 land = vec3(0.25,0.45,0.2);

    if(uv.y < hill){
        if(uv.y < 0.22){
            // river
            float w = fbm(vec2(uv.x*12.0 - t*3.0, uv.y*6.0));
            vec3 water = mix(vec3(0.05,0.2,0.45), vec3(0.2,0.5,0.7), w);
            float wave = sin(uv.x*40.0 - t*6.0)*0.02 + sin(uv.x*20.0 + t*3.0)*0.015;
            water += wave;
            col = water;
        } else {
            col = land*(0.6+0.4*uv.y);
        }
    }

    // waterfall center
    float wc = abs(uv.x-0.5);
    if(wc < 0.07 && uv.y > 0.22 && uv.y < hill){
        float flow = fbm(vec2(uv.x*30.0, uv.y*6.0 + t*8.0)); // downward flow
        vec3 wcol = mix(vec3(0.6,0.78,0.95), vec3(0.9,0.97,1.0), flow);
        float foam = smoothstep(0.65,0.85, flow);
        wcol = mix(wcol, vec3(1.0), foam*0.6);
        col = wcol;
        if(uv.y < 0.3){
            float mist = (0.3-uv.y)*3.0;
            col = mix(col, vec3(0.9,0.95,1.0), mist*0.5);
        }
    }

    // rocks
    float rock = fbm(vec2(uv.x*20.0, uv.y*20.0));
    if(rock>0.72 && uv.y<0.25) col = mix(col, vec3(0.3,0.3,0.35),0.6);

    fragColor = vec4(col,1.0);
}
