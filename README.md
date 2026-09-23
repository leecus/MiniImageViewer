# 看图 MiniImageViewer

一个 Windows 原生看图软件 —— **200KB 单文件、零依赖、纯 Win32 + GDI+**，无任何第三方库。

丝滑缩放、自由拖动、文件关联，小而美。

## 功能

- 🖱️ 滚轮以鼠标为中心缩放（0.01x ~ 64x），逐帧插值平滑动画
- ✋ 任意缩放比例下自由拖动平移，查看细节
- 双击在「适应窗口 / 100%」之间切换
- `←` `→` 浏览同目录上一张 / 下一张
- `F` 适应窗口 · `0` 回到 100% · `Ctrl+O` 打开 · `Esc` 关闭
- 图片直接拖进窗口即可打开；支持命令行参数 `ImageViewer.exe 图片路径`
- 按 `A` 一键注册 / 取消文件关联（用户级注册表，无需管理员权限），右键图片"打开方式"即可选用
- 暗色背景 + 半透明状态栏（文件名 / 分辨率 / 缩放比）
- 支持 jpg / jpeg / png / bmp / gif / tif / tiff / ico / wmf / emf

## 构建

只需 MinGW-w64（g++ 8.1+ 与 windres），一条命令：

```bat
windres app.rc -O coff -o app.res
g++ -O2 -std=c++17 -municode -mwindows -s -static -o ImageViewer.exe ImageViewer.cpp app.res ^
    -lgdiplus -lgdi32 -luser32 -lshell32 -lcomdlg32 -lole32 -ladvapi32 -lshlwapi
```

产物为静态链接单文件 exe（约 200KB），仅依赖系统自带 DLL，任何 Win10/11 直接运行。

## 文件说明

| 文件 | 说明 |
|------|------|
| `ImageViewer.cpp` | 全部源码（约 500 行，单文件） |
| `app.ico` / `app.rc` | 应用图标及资源脚本 |
| `build.bat` | 一键构建脚本 |

## 实现要点

- 双缓冲绘制防闪烁，按可视区域裁剪源图绘制，大图高倍缩放也不卡
- 缩放动画：16ms 定时器 + 指数插值，缩放锚点钉在鼠标处不漂移
- 图片读入内存后经 `IStream` 解码，不锁定文件
- `InterpolationModeHighQualityBicubic`（缩小）/ `NearestNeighbor`（放大）动态切换

## License

MIT
