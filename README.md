个人练手项目，如果帮助到你的话请点一个小心心，是对我的鼓励。
#This is a personal practice project. If you find it helpful, please give it a star, it means a lot to me.
# libeventServer‑TcpImageTransfer
基于 libevent 的跨平台 TCP 图像传输服务，实现服务端图像生成、多客户端接入、心跳保活、断线自动重连功能。服务端最多支持 10 个客户端同时连接，传输 640×480 8bit 灰度图像。

项目概述
## Project Overview
本项目分为服务端 (server) 与 PC 客户端 (client)，自定义 16 字节二进制协议头，解决 TCP 粘包半包问题。
This project consists of a server and a PC‑side client. A custom 16‑byte binary protocol header is designed to resolve TCP sticky packets and partial packet issues.

Server：后台线程定时生成灰度测试图像，条件变量广播新帧信号；每个客户端分配独立控制线程与图像发送线程，处理心跳、请求图像、停止图像、设备控制等指令。
**Server**: Background threads periodically generate grayscale test images, and broadcast new‑frame signals via condition variables. Each client is assigned an independent control thread and an image sending thread to handle commands including heartbeat, image request, image transmission stop, and device control.

Client：TCP 连接服务端，3 秒周期发送心跳检测链路；支持请求 / 停止图像接收；网络异常、读超时自动断线重连；可保存原始灰度图像文件。
**Client**: Establishes TCP connection to the server, and sends heartbeat packets every 3 seconds for link detection. It supports requesting / stopping image reception. Automatic disconnection and reconnection will be triggered on network exceptions or read timeout. Raw grayscale image files can be saved locally.



 本项目是本人课程学习的实践练习，全部内容均不用于商业盈利。项目实现过程中借鉴、使用了大量优秀的开源库与公开技术资料，在此向所有开源作者致以诚挚的感谢。若项目中存在因信息疏漏，在不知情的情况下使用了他人受版权保护的内容，恳请相关作者与我取得联系，我会及时修正补充相关版权说明。今后我也会加强对开源协议、版权规范的学习，避免无意侵权问题的发生。 

This project is a practical exercise for my university coursework. All content is non‑commercial and not intended for profit.

In the course of implementation, this project draws on and utilizes numerous excellent open‑source libraries and publicly‑available technical materials. I would like to extend my sincere gratitude to all open‑source authors for their contributions.

If any copyright‑protected work belonging to third parties has been unintentionally used without proper attribution due to omissions, please feel free to contact me. I will promptly revise and supplement the relevant copyright statements.

I will further deepen my understanding of open‑source licenses and copyright standards to prevent unintentional copyright infringements in future work.