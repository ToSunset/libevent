#pragma once
/* 客户端管理器：固定容量的客户端会话表（最多 kMaxClients 个），
 * 用 unique_ptr 持有会话对象；会话销毁由其自身控制线程触发。 */

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <event2/util.h>

#include "../common/protocol.h"
#include "client_session.h"

namespace cam {

class ImageSource;

class ClientManager {
public:
    explicit ClientManager(ImageSource* source) : source_(source) {}
    ~ClientManager();

    /* 新建并登记会话：表满/失败返回 nullptr */
    ClientSession* add(evutil_socket_t fd);

    /* 会话关闭回调（在会话自己的控制线程内调用），负责摘表并销毁 */
    void onClosed(ClientSession* s);

    int count() const { return count_.load(); }

private:
    ImageSource* source_ = nullptr;
    std::mutex   lock_;
    std::array<std::unique_ptr<ClientSession>, kMaxClients> slots_;
    std::atomic<int> count_{0};
};

}  /* namespace cam */
