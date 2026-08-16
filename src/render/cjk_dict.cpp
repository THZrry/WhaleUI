/* cjk_dict.cpp - runtime CJK dictionary loaded from res/whaleui_dict.bin.
 *
 * Format (all integers little-endian):
 *     magic   "WUID" (4 bytes)
 *     version u32 LE (1)
 *     group_count u32 LE
 *     per group:
 *         u8  head_len, head bytes        (UTF-8 head character)
 *         u16 tail_count
 *         u32 tails_blob_len
 *         tails_blob: tail_count × (u8 len + bytes)
 *
 * A word is present when its head group exists and its tail (word minus the
 * head) appears in that group's blob. Loaded once, kept as raw bytes +
 * per-group offsets: no per-word allocations, ~2 MB resident.
 */

#include "render/cjk_dict.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const unsigned int kVersion = 1;

struct Group
{
    std::string head;
    size_t blob_off; /* offset of the tails blob inside g_blob */
    int tail_count;
};

std::vector<unsigned char> g_blob;   /* raw tails blobs, concatenated */
std::vector<Group> g_groups;         /* head -> tails blob (sorted by head) */
bool g_loaded = false;
bool g_failed = false;

/* tiny built-in fallback so word jumps still work without the resource */
const char* const kFallback[] = {
    "你好", "世界", "中国", "人民", "我们", "他们", "你们", "自己", "大家",
    "这个", "那个", "这些", "那些", "什么", "怎么", "为什么", "因为", "所以",
    "但是", "如果", "虽然", "而且", "或者", "还是", "就是", "只是", "可是",
    "然后", "现在", "目前", "将来", "过去", "以前", "以后", "今天", "明天",
    "昨天", "早上", "中午", "晚上", "上午", "下午", "时候", "时间", "天气",
    "工作", "学习", "生活", "问题", "方法", "方案", "过程", "结果", "原因",
    "情况", "事情", "地方", "地址", "位置", "方向", "速度", "数量", "质量",
    "价格", "成本", "市场", "公司", "企业", "工厂", "商店", "银行", "医院",
    "学校", "老师", "学生", "同学", "朋友", "家人", "父母", "孩子", "大人",
    "男人", "女人", "先生", "女士", "医生", "律师", "警察", "司机", "工人",
    "国家", "城市", "北京", "上海", "广州", "深圳", "农村", "街道", "道路",
    "汽车", "火车", "飞机", "轮船", "地铁", "公交", "旅行", "旅游", "饭店",
    "餐厅", "酒店", "食物", "饮料", "水果", "蔬菜", "米饭", "面条", "面包",
    "鸡蛋", "牛奶", "咖啡", "衣服", "裤子", "鞋子", "眼镜", "手表", "手机",
    "电脑", "键盘", "鼠标", "屏幕", "网络", "网站", "软件", "硬件", "系统",
    "程序", "文件", "数据", "信息", "内容", "图片", "照片", "视频", "音乐",
    "电影", "电视", "新闻", "文章", "标题", "文字", "字体", "颜色", "背景",
    "边框", "阴影", "大小", "开始", "结束", "继续", "停止", "完成", "实现",
    "改变", "增加", "减少", "提高", "降低", "发展", "解决", "处理", "管理",
    "控制", "操作", "使用", "应用", "设计", "开发", "建设", "生产", "制造",
    "销售", "购买", "支付", "收到", "发送", "接收", "保存", "删除", "修改",
    "创建", "打开", "关闭", "输入", "输出", "显示", "隐藏", "移动", "复制",
    "粘贴", "剪切", "选择", "取消", "确认", "同意", "拒绝", "接受", "反对",
    "支持", "希望", "需要", "应该", "必须", "能够", "愿意", "喜欢", "知道",
    "认识", "理解", "明白", "记得", "忘记", "感谢", "道歉", "帮助", "保护",
    "寻找", "发现", "发明", "创造", "想象", "思考", "判断", "决定", "计划",
    "安排", "准备", "出发", "到达", "离开", "回来", "回去", "进来", "出去",
    "走路", "跑步", "唱歌", "跳舞", "写字", "阅读", "说话", "讨论", "商量",
    "允许", "禁止", "警告", "提醒", "建议", "要求", "命令", "请求", "邀请",
    "检查", "测试", "计算", "测量", "统计", "分析", "比较", "分类", "整理",
    "收集", "组合", "分解", "连接", "启动", "重启", "更新", "升级", "安装",
    "卸载", "注册", "登录", "退出", "密码", "账号", "用户", "权限", "安全",
    "隐私", "设置", "功能", "菜单", "按钮", "图标", "界面", "布局", "样式",
    "主题", "模式", "状态", "属性", "参数", "配置", "默认", "高级", "简单",
    "复杂", "重要", "紧急", "普通", "特殊", "正常", "异常", "错误", "正确",
    "成功", "失败", "有效", "无效", "可能", "大概", "也许", "肯定", "一定",
    "完全", "部分", "全部", "整体", "细节", "具体", "一般", "特别", "非常",
    "所有", "每个", "任何", "无论", "不管", "只要", "除非", "一旦", "即使",
    "尽管", "因此", "于是", "同时", "此外", "另外", "例如", "比如", "甚至",
    "尤其", "逐渐", "突然", "立即", "马上", "立刻", "经常", "总是", "很少",
    "偶尔", "一直", "始终", "永远", "曾经", "刚刚", "刚才", "已经", "正在",
    "将要", "几乎", "大约", "左右", "之间", "之中", "之内", "之外", "以上",
    "以下", "之前", "之后", "分别", "各自", "互相", "相互", "共同", "单独",
    "一起", "一同", "一块", "一边", "一面", "全部",
};
const int kFallbackCount = static_cast<int>(
    sizeof(kFallback) / sizeof(kFallback[0]));

bool fallback_has(const std::string& w)
{
    for (int i = 0; i < kFallbackCount; ++i) {
        if (w == kFallback[i]) {
            return true;
        }
    }
    return false;
}

bool load_file(const char* path)
{
    SDL_IOStream* io = SDL_IOFromFile(path, "rb");
    if (!io) {
        return false;
    }
    Sint64 len = SDL_GetIOSize(io);
    if (len <= 12 || len > 64 * 1024 * 1024) {
        SDL_CloseIO(io);
        return false;
    }
    std::vector<unsigned char> buf(static_cast<size_t>(len));
    if (SDL_ReadIO(io, buf.data(), static_cast<size_t>(len)) !=
        static_cast<size_t>(len)) {
        SDL_CloseIO(io);
        return false;
    }
    SDL_CloseIO(io);

    auto rd32 = [&buf](size_t off) -> unsigned int {
        if (off + 4 > buf.size()) {
            return 0;
        }
        return static_cast<unsigned int>(buf[off]) |
               (static_cast<unsigned int>(buf[off + 1]) << 8) |
               (static_cast<unsigned int>(buf[off + 2]) << 16) |
               (static_cast<unsigned int>(buf[off + 3]) << 24);
    };
    if (buf.size() < 12 || std::memcmp(buf.data(), "WUID", 4) != 0) {
        return false;
    }
    if (rd32(4) != kVersion) {
        return false;
    }
    unsigned int groups = rd32(8);
    size_t off = 12;
    std::vector<Group> gs;
    gs.reserve(groups);
    std::vector<unsigned char> blob;
    blob.reserve(buf.size());
    for (unsigned int gi = 0; gi < groups; ++gi) {
        if (off >= buf.size()) {
            return false;
        }
        unsigned int hl = buf[off++];
        if (off + hl + 6 > buf.size()) {
            return false;
        }
        std::string head(reinterpret_cast<const char*>(buf.data() + off), hl);
        off += hl;
        unsigned int tc = static_cast<unsigned int>(buf[off]) |
                          (static_cast<unsigned int>(buf[off + 1]) << 8);
        unsigned int bl = rd32(off + 2);
        off += 6;
        if (off + bl > buf.size()) {
            return false;
        }
        Group g;
        g.head = head;
        g.blob_off = blob.size();
        g.tail_count = static_cast<int>(tc);
        blob.insert(blob.end(), buf.begin() + static_cast<long>(off),
                    buf.begin() + static_cast<long>(off + bl));
        off += bl;
        gs.push_back(g);
    }
    g_blob.swap(blob);
    g_groups.swap(gs);
    return true;
}

void ensure_loaded()
{
    if (g_loaded) {
        return;
    }
    g_loaded = true;
    const char* candidates[] = {
        "whaleui_dict.bin",        /* cwd */
        "res/whaleui_dict.bin",    /* cwd/res */
        nullptr,                   /* exe dir (filled below) */
    };
    char exe_path[1024];
    const char* base = SDL_GetBasePath();
    if (base) {
        std::snprintf(exe_path, sizeof(exe_path), "%swhaleui_dict.bin", base);
        candidates[2] = exe_path;
        if (load_file(exe_path)) {
            return;
        }
    }
    if (load_file(candidates[0]) || load_file(candidates[1])) {
        return;
    }
    g_failed = true;
    std::fprintf(stderr,
                 "whaleui: cjk dictionary not found (whaleui_dict.bin / "
                 "res/whaleui_dict.bin); using the built-in mini list\n");
}

} // namespace

extern "C" int whaleui_cjk_dict_ready(void)
{
    ensure_loaded();
    return g_failed ? 0 : 1;
}

extern "C" int whaleui_cjk_dict_load(const char* path)
{
    if (!path) {
        return -1;
    }
    std::vector<Group> gs;
    std::vector<unsigned char> blob;
    gs.swap(g_groups);
    blob.swap(g_blob);
    if (load_file(path)) {
        g_loaded = true;
        g_failed = false;
        return 0;
    }
    gs.swap(g_groups);
    blob.swap(g_blob);
    return -1;
}

extern "C" int whaleui_cjk_dict_has(const char* w, int len)
{
    ensure_loaded();
    if (!w || len <= 0) {
        return 0;
    }
    std::string word(w, static_cast<size_t>(len));
    if (g_failed) {
        return fallback_has(word) ? 1 : 0;
    }
    /* head = first UTF-8 char */
    size_t hl = 1;
    unsigned char c0 = static_cast<unsigned char>(word[0]);
    if ((c0 & 0xE0) == 0xC0) {
        hl = 2;
    } else if ((c0 & 0xF0) == 0xE0) {
        hl = 3;
    } else if ((c0 & 0xF8) == 0xF0) {
        hl = 4;
    }
    if (hl >= word.size()) {
        return 0; /* single char: never a dict word */
    }
    std::string head = word.substr(0, hl);
    std::string tail = word.substr(hl);
    /* binary search over the codepoint-sorted groups (UTF-8 order) */
    int lo = 0, hi = static_cast<int>(g_groups.size()) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = head.compare(g_groups[static_cast<size_t>(mid)].head);
        if (cmp == 0) {
            const Group& g = g_groups[static_cast<size_t>(mid)];
            size_t p = g.blob_off;
            for (int i = 0; i < g.tail_count; ++i) {
                if (p >= g_blob.size()) {
                    return 0;
                }
                unsigned int tl = g_blob[p++];
                if (p + tl > g_blob.size()) {
                    return 0;
                }
                if (tl == tail.size() &&
                    std::memcmp(g_blob.data() + p, tail.data(), tl) == 0) {
                    return 1;
                }
                p += tl;
            }
            return 0;
        }
        if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}
