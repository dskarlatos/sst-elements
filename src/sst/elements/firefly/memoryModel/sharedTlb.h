
class SharedTlb {
    std::string m_prefix;
    const char* prefix() { return m_prefix.c_str(); }

    typedef std::function<void(MemReq*, uint64_t)> Callback; 
public:
    SharedTlb( SimpleMemoryModel& model, Output& dbg, int id, int size, int pageSize, int tlbMissLat_ns, int numWalkers ) :
        m_model(model), m_dbg(dbg), m_tlbMissLat_ns(tlbMissLat_ns), m_numWalkers(numWalkers), m_pageMask( ~(pageSize - 1) ),
        m_cache(size), m_total(0), m_hitCnt(0), m_numLookups(0), m_maxNumLookups(numWalkers), m_cacheSize(size)
        //m_cache(4,128,pageSize), m_total(0), m_hitCnt(0), m_numLookups(0), m_maxNumLookups(numWalkers)
        //m_cache(512,1,pageSize), m_total(0), m_hitCnt(0), m_numLookups(0), m_maxNumLookups(numWalkers)
    {
        m_prefix = "@t:" + std::to_string(id) + ":SimpleMemoryModel::SharedTlb::@p():@l ";

        m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK,"tlbSize=%d, pageMask=%#" PRIx64 ", numWalkers=%d\n",
                        size, m_pageMask, numWalkers );
    }

    uint64_t lookup( MemReq* req, Callback callback, bool flag = false ) {

        ++m_total;
        uint64_t pageAddr = processPageAddr(req); 
        uint64_t physAddr = processPhysAddr(req);

        if ( 0 == m_cacheSize ) {
            ++m_hitCnt;
            return physAddr;
        }


        if (  m_cache.isValid( pageAddr ) ) {
            m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK, "Hit: virtAddr=%#" PRIx64 " physAddr=%#" PRIx64 " pageAddr=%#" PRIx64"\n", 
                req->addr, physAddr, pageAddr );
            ++m_hitCnt;
            return physAddr;
        } else {
            if ( m_pendingMap.find(pageAddr) != m_pendingMap.end() ) {
                m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK, "Pending: virtAddr=%#" PRIx64 " physAddr=%#" PRIx64 " pageAddr=%#" PRIx64"\n", 
                    req->addr, physAddr, pageAddr );
                m_pendingMap[pageAddr].push_back( std::make_pair( req, callback ) );
            } else {
                if ( m_numLookups < m_maxNumLookups ) {
                    ++m_numLookups;
                    m_pendingMap[pageAddr];
                    m_model.schedCallback( m_tlbMissLat_ns, std::bind( &SharedTlb::resolved, this, req, callback )); 
                    m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK, "Schedule: virtAddr=%#" PRIx64 " physAddr=%#" PRIx64 " pageAddr=%#" PRIx64"\n", 
                        req->addr, physAddr, pageAddr );
                } else {
                    m_pendingLookups.push_back( std::make_pair( req, callback ) );
                    m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK, "Blocked: virtAddr=%#" PRIx64 " physAddr=%#" PRIx64 " pageAddr=%#" PRIx64"\n", 
                        req->addr, physAddr, pageAddr );
                }
            }
        }  

        return -1;
    }

    void printStats() {
        if ( m_total ) {
            m_dbg.output("SharedTlb: total requests %" PRIu64 " %f percent hits\n",
                        m_total, (float)m_hitCnt/(float)m_total);
        }
        //m_cache.printStats( m_dbg );
    }
private:

    std::deque< std::pair< MemReq*, Callback > > m_pendingLookups;
    int m_cacheSize;
    int m_numLookups;
    int m_maxNumLookups;
    uint64_t m_total;
    uint64_t m_hitCnt;

    void resolved( MemReq* req, Callback callback  ) {
        uint64_t pageAddr = processPageAddr(req); 
        uint64_t physAddr = processPhysAddr(req);
        m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK,
                        "virtAddr=%#" PRIx64 " physAddr=%#" PRIx64 " pageAddr=%#" PRIx64"\n", req->addr, physAddr, pageAddr );

        --m_numLookups;
        //m_cache.evict( pageAddr );
        m_cache.evict(  );
        m_cache.insert( pageAddr ); 
        callback( req, physAddr );
        while( ! m_pendingMap[pageAddr].empty() ) {
                             
            m_pendingMap[pageAddr].front().second( m_pendingMap[pageAddr].front().first, physAddr );
            m_pendingMap[pageAddr].pop_front();
         }
         m_pendingMap.erase(pageAddr);

        while ( ! m_pendingLookups.empty() ) {

            req = m_pendingLookups.front().first;
            callback = m_pendingLookups.front().second;
            
            uint64_t pageAddr = processPageAddr(req); 

            m_pendingLookups.pop_front();

            if ( m_cache.isValid( pageAddr )  ) {
                callback( req, processPhysAddr(req) );
            } else if ( m_pendingMap.find(pageAddr) != m_pendingMap.end() ) {
                m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK, "Pending: virtAddr=%#" PRIx64 " physAddr=%#" PRIx64 " pageAddr=%#" PRIx64"\n", 
                        req->addr, physAddr, pageAddr );
                m_pendingMap[pageAddr].push_back( std::make_pair( req, callback ) );
            } else {
                ++m_numLookups;
                m_pendingMap[pageAddr];
                m_model.schedCallback( m_tlbMissLat_ns, std::bind( &SharedTlb::resolved, this, req, callback )); 
                m_dbg.verbosePrefix(prefix(),CALL_INFO,1,SHARED_TLB_MASK, "Schedule: virtAddr=%#" PRIx64 " physAddr=%#" PRIx64 " pageAddr=%#" PRIx64"\n", 
                req->addr, physAddr, pageAddr );
                break;
            }
        }
    }

    uint64_t processPageAddr( MemReq* req ) {
        return getPageAddr( req->addr ) | (uint64_t) req->pid << 56;
    }
    uint64_t processPhysAddr( MemReq* req ) {
        return req->addr | (uint64_t) req->pid << 56;
    }

    Hermes::Vaddr getPageAddr(Hermes::Vaddr addr ) {
        return addr & m_pageMask;
    }

    std::unordered_map< uint64_t, std::deque< std::pair< MemReq*, Callback > > > m_pendingMap;
    SimpleMemoryModel& m_model;
    Output& m_dbg;
    int m_tlbMissLat_ns;
    int m_numWalkers;
    uint64_t m_pageMask;
    //NWayCache       m_cache;
    Cache       m_cache;
};
