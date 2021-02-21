#include <cascade/cascade.hpp>
#include <cascade/service.hpp>
#include <cascade/service_types.hpp>
#include <cascade/object.hpp>
#include <sys/prctl.h>
#include <derecho/conf/conf.hpp>
#include <derecho/utils/logger.hpp>
#include <dlfcn.h>
#include <type_traits>

#define PROC_NAME "cascade_server"

using namespace derecho::cascade;

#ifndef NDEBUG
inline void dump_layout(const json& layout) {
    int tid = 0;
    for (const auto& pertype:layout) {
        int sidx = 0;
        for (const auto& persubgroup:pertype ) {
            dbg_default_trace("subgroup={}.{},layout={}.",tid,sidx,persubgroup.dump());
            sidx ++;
        }
        tid ++;
    }
}
#endif//NDEBUG

template <typename CascadeType>
class CascadeServiceCDPO: public CriticalDataPathObserver<CascadeType> {
    virtual void operator() (const uint32_t sgidx,
                             const uint32_t shidx,
                             const typename CascadeType::KeyType& key,
                             const typename CascadeType::ObjectType& value,
                             ICascadeContext* cascade_ctxt) {
        if constexpr (std::is_convertible<typename CascadeType::KeyType,std::string>::value) {
            auto* ctxt = dynamic_cast<CascadeContext<VolatileCascadeStoreWithStringKey,PersistentCascadeStoreWithStringKey>*>(cascade_ctxt);
            OffCriticalDataPathObserver* ocdpo_ptr = nullptr;
            size_t pos = key.rfind('/');
            std::string prefix;
            if (pos != std::string::npos) {
                prefix = key.substr(0,pos);
            }
            if ((ctxt != nullptr) && (ocdpo_ptr = ctxt->get_prefix_handler(prefix))) {
                Action action(key,value.get_version(),ocdpo_ptr,std::make_unique<typename CascadeType::ObjectType>(value));
                ctxt->post(std::move(action));
            }
        }
    }
};

int main(int argc, char** argv) {
    // set proc name
    if( prctl(PR_SET_NAME, PROC_NAME, 0, 0, 0) != 0 ) {
        dbg_default_warn("Cannot set proc name to {}.", PROC_NAME);
    }
    dbg_default_trace("set proc name to {}", PROC_NAME);
    // load configuration
    auto group_layout = json::parse(derecho::getConfString(CONF_GROUP_LAYOUT));

#ifndef NDEBUG
    dbg_default_trace("load layout:");
    dump_layout(group_layout);
#endif//NDEBUG

    CascadeServiceCDPO<VolatileCascadeStoreWithStringKey> cdpo_vcss;
    CascadeServiceCDPO<PersistentCascadeStoreWithStringKey> cdpo_pcss;

    auto vcss_factory = [&cdpo_vcss](persistent::PersistentRegistry*, derecho::subgroup_id_t, ICascadeContext* context_ptr) {
        return std::make_unique<VolatileCascadeStoreWithStringKey>(&cdpo_vcss,context_ptr);
    };
    auto pcss_factory = [&cdpo_pcss](persistent::PersistentRegistry* pr, derecho::subgroup_id_t, ICascadeContext* context_ptr) {
        return std::make_unique<PersistentCascadeStoreWithStringKey>(pr,&cdpo_pcss,context_ptr);
    };
    dbg_default_trace("starting service...");
    Service<VolatileCascadeStoreWithStringKey,PersistentCascadeStoreWithStringKey>::start(group_layout,
            {&cdpo_vcss,&cdpo_pcss},
            vcss_factory,pcss_factory);
    dbg_default_trace("started service, waiting till it ends.");
    std::cout << "Press Enter to Shutdown." << std::endl;
    std::cin.get();
    // wait for service to quit.
    Service<VolatileCascadeStoreWithStringKey,PersistentCascadeStoreWithStringKey>::shutdown(false);
    dbg_default_trace("shutdown service gracefully");
    // you can do something here to parallel the destructing process.
    Service<VolatileCascadeStoreWithStringKey,PersistentCascadeStoreWithStringKey>::wait();
    dbg_default_trace("Finish shutdown.");

    return 0;
}
