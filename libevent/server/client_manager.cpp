#include "client_manager.h"

#include "common/logger.h"
#include "common/protocol.h"
#include "client_session.h"

namespace cam {

ClientManager::~ClientManager()
{
    /* unique_ptr 的析构需要 ClientSession 完整类型，故在 .cpp 中定义 */
}

ClientSession* ClientManager::add(evutil_socket_t fd)
{
    std::lock_guard<std::mutex> lk(lock_);

    if (count_.load() >= kMaxClients) {
        LOG_WARN("[server] 客户端数已达 %d，拒绝新连接", kMaxClients);
        return nullptr;
    }

    for (int i = 0; i < kMaxClients; i++) {
        if (!slots_[static_cast<size_t>(i)]) {
            std::unique_ptr<ClientSession> session(new ClientSession(this, source_, fd));
            session->setSlot(i);
            slots_[static_cast<size_t>(i)] = std::move(session);
            count_.fetch_add(1);
            return slots_[static_cast<size_t>(i)].get();
        }
    }
    return nullptr;   /* 表满（理论上不会发生） */
}

void ClientManager::onClosed(ClientSession* s)
{
    std::lock_guard<std::mutex> lk(lock_);

    const int idx = s->slot();
    if (idx >= 0 && idx < kMaxClients &&
        slots_[static_cast<size_t>(idx)].get() == s) {
        slots_[static_cast<size_t>(idx)].reset();   /* 销毁会话 */
        count_.fetch_sub(1);
    }
}

}  /* namespace cam */
