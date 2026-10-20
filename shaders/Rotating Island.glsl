// 旋转相机视角下的 Procedural 山地景观
// 包含天空、白云、山体材质、相机环绕旋转

#define PI 3.14159265358979323846

// 基础随机函数
float random(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

// 2D 值噪声
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    float a = random(i);
    float b = random(i + vec2(1.0, 0.0));
    float c = random(i + vec2(0.0, 1.0));
    float d = random(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 多层分形噪声，生成山体高度
float mountain(vec2 p) {
    float h = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < 6; i++) {
        h += noise(p * freq) * amp;
        freq *= 2.0;
        amp *= 0.5;
    }
    return h;
}

// 地形高度场：y 高于高度为空气，低于高度为山体
float map(vec3 p) {
    float h = mountain(p.xz * 0.015) * 35.0 + 12.0;
    return p.y - h;
}

// 计算山体法线
vec3 getNormal(vec3 p) {
    vec2 e = vec2(0.1, 0.0);
    return normalize(vec3(
        map(p + e.xyy) - map(p - e.xyy),
        map(p + e.yxy) - map(p - e.yxy),
        map(p + e.yyx) - map(p - e.yyx)
    ));
}

// 光线步进求地形交点
vec3 raymarch(vec3 ro, vec3 rd) {
    float t = 0.0;
    for (int i = 0; i < 120; i++) {
        vec3 p = ro + rd * t;
        float d = map(p);
        if (d < 0.1) return p;
        t += d;
        if (t > 400.0) break;
    }
    return vec3(-1.0);
}

// 天空与白云
vec3 sky(vec3 rd) {
    // 蓝色天空渐变
    float h = clamp(rd.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 col = mix(vec3(0.75, 0.85, 0.95), vec3(0.35, 0.55, 0.85), h);

    // 简单云层
    vec2 cloudUv = rd.xz / max(rd.y, 0.01) * 0.02 + vec2(iTime * 0.01, 0.0);
    float cloud = noise(cloudUv * 3.0);
    cloud += noise(cloudUv * 6.0) * 0.5;
    cloud = smoothstep(0.5, 1.2, cloud);
    col = mix(col, vec3(1.0), cloud * 0.6);

    return col;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (fragCoord * 2.0 - iResolution.xy) / iResolution.y;

    // 相机环绕旋转
    float an = iTime * 0.08;        // 旋转速度
    float r = 220.0;                // 旋转半径
    float h = 60.0;                 // 相机高度
    vec3 ro = vec3(sin(an) * r, h, cos(an) * r);
    vec3 target = vec3(0.0, 25.0, 0.0);

    // 相机矩阵
    vec3 f = normalize(target - ro);
    vec3 s = normalize(cross(vec3(0.0, 1.0, 0.0), f));
    vec3 u = cross(f, s);
    vec3 rd = normalize(uv.x * s + uv.y * u + 1.2 * f);

    vec3 col = sky(rd);
    vec3 hit = raymarch(ro, rd);

    // 如果击中山体
    if (hit.x > -0.5) {
        vec3 n = getNormal(hit);
        vec3 light = normalize(vec3(0.6, 0.9, 0.4));
        float diff = clamp(dot(n, light), 0.2, 1.0);

        // 山体材质：岩土 + 雪线
        float heightN = clamp((hit.y - 12.0) / 35.0, 0.0, 1.0);
        vec3 rock = vec3(0.45, 0.32, 0.2);
        vec3 snow = vec3(0.92, 0.9, 0.88);
        vec3 mountainCol = mix(rock, snow, smoothstep(0.55, 0.85, heightN));

        // 近处清晰，远处雾化
        float dist = length(hit - ro);
        float fog = exp(-dist * 0.008);
        vec3 skyCol = sky(rd);

        col = mix(skyCol, mountainCol * diff, fog);
    }

    // 简单色调映射
    col = pow(col, vec3(0.75));

    fragColor = vec4(col, 1.0);
}

