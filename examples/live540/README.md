# 540×960@30 逐帧基线

从本目录运行，输入是 8-bit I420，输出目录必须是新目录。

```sh
./scripts/build.sh
./scripts/run.sh sample.i420 trial-01
```

独立线程按 30 fps 入队。固定基础 QP 32，ultrafast、4 线程、过去参考、OWF=0、每 60 帧 IDR。每一行 CSV 对应一个实际返回的压缩帧，summary.json 汇总入队到出帧 p95、编码调用耗时与最长出帧间隔。客户使用的受限 CRF 控制器与强制 I 策略位于 NakiLiveTranscode 服务，本工具单独测固定 QP。
