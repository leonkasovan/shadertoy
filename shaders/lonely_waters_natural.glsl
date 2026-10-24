// Copied from https://www.shadertoy.com/view/NlKGWK
// Created by https://www.shadertoy.com/user/Tater

//Set to 2.0 for AA
#define AA 1.0

#define STEPS 80.0
#define MDIST 35.0
#define pi 3.1415926535
#define rot(a) mat2(cos(a),sin(a),-sin(a),cos(a))
#define sat(a) clamp(a,0.0,1.0)

#define ITERS_TRACE 9
#define ITERS_NORM 20

#define HOR_SCALE 1.1
#define OCC_SPEED 1.4
#define DX_DET 0.65

#define FREQ 0.6
#define HEIGHT_DIV 2.5
#define WEIGHT_SCL 0.8
#define FREQ_SCL 1.2
#define TIME_SCL 1.095
#define WAV_ROT 1.21
#define DRAG 0.9
#define SCRL_SPEED 1.5
vec2 scrollDir = vec2(1,1);


//Built with some ideas from
//https://www.shadertoy.com/view/wldBRf
//https://www.shadertoy.com/view/ssG3Wt
//https://www.shadertoy.com/view/4dBcRD
//https://www.shadertoy.com/view/Xdlczl
vec2 wavedx(vec2 wavPos, int iters, float t){
    vec2 dx = vec2(0);
    vec2 wavDir = vec2(1,0);
    float wavWeight = 1.0;
    wavPos+= t*SCRL_SPEED*scrollDir;
    wavPos*= HOR_SCALE;
    float wavFreq = FREQ;
    float wavTime = OCC_SPEED*t;
    for(int i=0;i<iters;i++){
        wavDir*=rot(WAV_ROT);
        float x = dot(wavDir,wavPos)*wavFreq+wavTime;
        float result = exp(sin(x)-1.)*cos(x);
        //if(result>0.) foam+=result*0.3;
        result*=wavWeight;
        dx+= result*wavDir/pow(wavWeight,DX_DET);
        wavFreq*= FREQ_SCL;
        wavTime*= TIME_SCL;
        wavPos-= wavDir*result*DRAG;
        wavWeight*= WEIGHT_SCL;
    }
    float wavSum = -(pow(WEIGHT_SCL,float(iters))-1.)*HEIGHT_DIV;
    return dx/pow(wavSum,1.-DX_DET);
}

float wave(vec2 wavPos, int iters, float t){
    float wav = 0.0;
    vec2 wavDir = vec2(1,0);
    float wavWeight = 1.0;
    wavPos+= t*SCRL_SPEED*scrollDir;
    wavPos*= HOR_SCALE;
    float wavFreq = FREQ;
    float wavTime = OCC_SPEED*t;
    for(int i=0;i<iters;i++){
        wavDir*=rot(WAV_ROT);
        float x = dot(wavDir,wavPos)*wavFreq+wavTime;
        float wave = exp(sin(x)-1.0)*wavWeight;
        wav+= wave;
        wavFreq*= FREQ_SCL;
        wavTime*= TIME_SCL;
        wavPos-= wavDir*wave*DRAG*cos(x);
        wavWeight*= WEIGHT_SCL;
    }
    float wavSum = -(pow(WEIGHT_SCL,float(iters))-1.)*HEIGHT_DIV;
    return wav/wavSum;
}

vec3 norm(vec3 p){
    vec2 wav = -wavedx(p.xz, ITERS_NORM, iTime);
    return normalize(vec3(wav.x,1.0,wav.y));
}

float map(vec3 p){
    float a = 0.;
    int steps = ITERS_TRACE;
    p.y-= wave(p.xz,steps,iTime);
    a = p.y;
    return a;
}

// NOTE: pal()/spc()/spec below are no longer used by this variant - every
// element now has an explicit natural color (see the COL_* block). They are
// left in place because they document where the original's red-dominant look
// came from and make it easy to A/B back to the cosine palette.
vec3 pal(float t, vec3 a, vec3 b, vec3 c, vec3 d){
    return a+b*cos(2.0*pi*(c*t+d));
}
vec3 spc(float n,float bright){
    return pal(n,vec3(bright),vec3(0.5),vec3(1.0),vec3(0.0,0.33,0.67));
}
// Sun removed from this variant: sunrot/COL_SUN are no longer referenced.
// Kept for reference / easy restore.
vec2 sunrot = vec2(-0.3,-0.25);

// NATURAL PALETTE -------------------------------------------------------------
// The original drives every element (sun, sky tint, water, subsurface glow) from
// spc() at a single red-dominant palette entry (~0.89,0.08,0.25), which is why
// the whole scene reads magenta/maroon with no blue channel to speak of. These
// replace that with per-element physically-plausible colors.
#define COL_SUN        vec3(1.00, 0.94, 0.80)  // warm white-yellow sun disc (unused: sun removed)
#define COL_SUN_HALO   vec3(1.00, 0.86, 0.60)  // yellow glow / cloud tint
// Sky stops retuned to match "2D Clouds.0.glsl", whose palette is a clean
// single-hue ramp (hue 210 deg, saturation 0.60-0.67):
//   skycolour2 (top)    = (0.4,0.7,1.0)  hue 210 sat 0.60 val 1.00
//   skycolour1 (bottom) = (0.2,0.4,0.6)  hue 210 sat 0.67 val 0.60
// The previous stops were lopsided - zenith over-saturated (0.87) and horizon
// washed out (0.33) - which is what made the sky read muddy cyan instead of
// clean blue. These keep both ends near the reference saturation while staying
// slightly deeper than 2D Clouds so the water reflection still has contrast.
#define COL_SKY_ZENITH vec3(0.16, 0.40, 0.80)  // hue 217 sat 0.80 val 0.80
#define COL_SKY_HORIZ  vec3(0.55, 0.74, 0.98)  // hue 214 sat 0.44 val 0.98
#define COL_SKY_TINT   vec3(0.52, 0.72, 0.96)  // blue cloud scattering
#define COL_WATER_DEEP vec3(0.02, 0.13, 0.26)  // deep sea blue
#define COL_WATER_LIT  vec3(0.05, 0.28, 0.40)  // sea green-blue in the light
#define COL_WATER_GLOW vec3(0.10, 0.42, 0.50)  // subsurface teal

// Original single-knob palette position (red-dominant). Unused here; see NOTE
// above. Change the color of the scene via the COL_* block instead.
float spec = 0.13;

vec3 sky(vec3 rd){
    vec3 rdo = rd;
    vec3 col = vec3(0);

    // Sun removed: the disc, its halo and the cloud-occlusion term are gone.
    // The rest of the sky (gradient + clouds) no longer needs the sun rotation,
    // so that transform was dropped too. Since sky() is also used for the water
    // reflection (sky(rfl)) and for skyrd, the sun is consistently absent from
    // both the sky and its reflection in the sea.

    // NATURAL: blue sky gradient (zenith -> horizon) as the base layer. The
    // original had no gradient; every direction inherited the red palette tone.
    // Gradient sharpened from *1.8 to *2.6 so the deep zenith colour is
    // actually reached on screen: the camera only sees up to rdo.y ~ 0.36, so at
    // *1.8 the ramp topped out around 0.76 and the zenith blue never showed.
    float horizonT = sat(rdo.y*2.6+0.05);
    col = mix(COL_SKY_HORIZ, COL_SKY_ZENITH, horizonT);
    col = mix(col, COL_SKY_TINT, sat(1.0-length(col))*0.12);

    //This code provides the implication of clouds :)
    float e = 0.;
    vec3 p = rdo;
    p.xz*=0.4;
    p.x+=iTime*0.007;
    for(float s=200.;s>10.;s*=0.8){
        p.xz*=rot(s);
        p+=s;
        e+=abs(dot(sin(p*s+iTime*0.02)/s,vec3(1.65)));
    }
    e*=smoothstep(0.5,0.4,e-0.095);

    // Clouds no longer need the sun-occlusion factor, so they simply add over
    // the gradient.
    col += (e)*smoothstep(-0.02,0.3,rdo.y)*0.8*COL_SUN_HALO*0.85;
    return (col);
}
void render( out vec4 fragColor, in vec2 fragCoord ){
    vec2 uv = (fragCoord-0.5*iResolution.xy)/min(iResolution.y,iResolution.x);
    vec3 col = vec3(0);
    vec3 ro = vec3(0,2.25,-3)*1.1;
    bool click = iMouse.z>0.;
    if(click){
        ro.yz*=rot(2.0*min(iMouse.y/iResolution.y-0.5,0.15));
        ro.zx*=rot(-7.0*(iMouse.x/iResolution.x-0.5));
    }
    vec3 lk = vec3(0,2,0);
    vec3 f = normalize(lk-ro);
    vec3 r = normalize(cross(vec3(0,1,0),f));
    vec3 rd = normalize(f*(0.9)+uv.x*r+uv.y*cross(f,r));

    float dO = 0.;
    bool hit = false;
    float d = 0.;
    vec3 p = ro;

    float tPln = -(ro.y-1.86)/rd.y;
    if(tPln>0.||click){
        if(!click)dO+=tPln;
        for(float i = 0.; i<STEPS; i++){
            p = ro+rd*dO;d = map(p);dO+=d;
            if(abs(d)<0.005||i>STEPS-2.0){
                hit = true;
                break;
            }
            if(dO>MDIST){
                dO = MDIST;
                break;
            }
        }
    }
    vec3 skyrd = sky(rd);
    if(hit){
        vec3 n = norm(p);
        vec3 rfl = reflect(rd,n);
        rfl.y = abs(rfl.y);
        float fres = clamp((pow(1. - max(0.0, dot(-n, rd)), 5.0)),0.0,1.0);

        col += sky(rfl)*fres*0.9;
        // Sun removed: the sub-surface sparkle (refracted ray vs sunDir) and the
        // rd2 sun-rotation below are gone, so there is no sun glitter on the sea
        // to contradict the sunless sky.
        vec3 waterCol;

        // NATURAL: sea body color. Depth-weighted blend of lit surface and deep
        // water, replacing the red palette entry that made the sea maroon.
        vec3 waterTint = mix(COL_WATER_DEEP, COL_WATER_LIT, sat(pow(min(p.y*0.7+0.9,1.8),4.)*0.5+0.35));
waterCol = waterTint*(0.4*pow(min(p.y*0.7+0.9,1.8),4.)*length(skyrd)*(rd.z*0.15+0.85));
col += waterCol*0.17;
//col+=smoothstep(0.95,1.55,wave(p.xz,25,iTime))*mix(waterCol*0.3,vec3(1),0.2)*0.2;

col = mix(col,skyrd,dO/MDIST);
}
else{
col += skyrd;
}
col = sat(col);
col = pow(col,vec3(0.87));
// Vignette coefficient reduced from 0.8 to 0.45: at 0.8 it was strongest at the
// top corners, which is exactly where the zenith blue reaches the screen, so it
// was crushing the part of the gradient that gives the sky its depth. Keeping a
// softer falloff preserves the framing without flattening the sky.
col *=1.0-0.45*pow(length(uv*vec2(0.8,1.)),2.7);
fragColor = vec4(col,1.0);

}

#define ZERO min(0.0,iTime)
void mainImage( out vec4 fragColor, in vec2 fragCoord )
{
    float px = 1.0/AA;
    vec4 col = vec4(0);

    if(AA==1.0) {render(col,fragCoord); fragColor = col; return;}

    for(float i = ZERO; i <AA; i++){
        for(float j = ZERO; j <AA; j++){
            vec4 col2;
            vec2 coord = vec2(fragCoord.x+px*i,fragCoord.y+px*j);
            render(col2,coord);
            col.rgb+=col2.rgb;
        }
    }
    col/=AA*AA;
    fragColor = vec4(col);
}
