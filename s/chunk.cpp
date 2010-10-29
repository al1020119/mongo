// @file chunk.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "chunk.h"
#include "config.h"
#include "grid.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../db/queryutil.h"
#include "cursors.h"
#include "strategy.h"

namespace mongo {

    inline bool allOfType(BSONType type, const BSONObj& o){
        BSONObjIterator it(o);
        while(it.more()){
            if (it.next().type() != type)
                return false;
        }
        return true;
    }

    // -------  Shard --------

    int Chunk::MaxChunkSize = 1024 * 1024 * 200;
    
    Chunk::Chunk( ChunkManager * manager )
      : _manager(manager),
        _lastmod(0), _modified(false), _dataWritten(0)
    {}

    Chunk::Chunk(ChunkManager * info , const BSONObj& min, const BSONObj& max, const Shard& shard)
      : _manager(info), _min(min), _max(max), _shard(shard),
        _lastmod(0), _modified(false), _dataWritten(0)
    {}

    string Chunk::getns() const {
        assert( _manager );
        return _manager->getns(); 
    }

    bool Chunk::contains( const BSONObj& obj ) const{
        return
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    bool ChunkRange::contains(const BSONObj& obj) const {
    // same as Chunk method
        return 
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    bool Chunk::minIsInf() const {
        return _manager->getShardKey().globalMin().woCompare( getMin() ) == 0;
    }

    bool Chunk::maxIsInf() const {
        return _manager->getShardKey().globalMax().woCompare( getMax() ) == 0;
    }

    BSONObj Chunk::_getExtremeKey( int sort ) const {
        ShardConnection conn( getShard().getConnString() , _manager->getns() );
        Query q;
        if ( sort == 1 ) {
            q.sort( _manager->getShardKey().key() );
        }
        else {
            // need to invert shard key pattern to sort backwards
            // TODO: make a helper in ShardKeyPattern?

            BSONObj k = _manager->getShardKey().key();
            BSONObjBuilder r;
            
            BSONObjIterator i(k);
            while( i.more() ) {
                BSONElement e = i.next();
                uassert( 10163 ,  "can only handle numbers here - which i think is correct" , e.isNumber() );
                r.append( e.fieldName() , -1 * e.number() );
            }
            
            q.sort( r.obj() );
        }
        
        // find the extreme key
        BSONObj end = conn->findOne( _manager->getns() , q );
        conn.done();
        
        if ( end.isEmpty() )
            return BSONObj();
        
        return _manager->getShardKey().extractKey( end );
    }

    void Chunk::pickMedianKey( BSONObj& medianKey ) const {
        // Ask the mongod holding this chunk to figure out the split points.
        ScopedDbConnection conn( getShard().getConnString() );
        BSONObj result;
        BSONObjBuilder cmd;
        cmd.append( "splitVector" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKey().key() );
        cmd.append( "min" , getMin() );
        cmd.append( "max" , getMax() );
        cmd.appendBool( "force" , true );
        BSONObj cmdObj = cmd.obj();

        if ( ! conn->runCommand( "admin" , cmdObj , result )){
            conn.done();
            ostringstream os;
            os << "splitVector command (median key) failed: " << result;
            uassert( 13503 , os.str() , 0 );
        }       

        BSONObjIterator it( result.getObjectField( "splitKeys" ) );
        if ( it.more() ){
            medianKey = it.next().Obj().getOwned();
        }

        conn.done();
    }

    void Chunk::pickSplitVector( vector<BSONObj>& splitPoints , int chunkSize /* bytes */, int maxPoints, int maxObjs ) const { 
        // Ask the mongod holding this chunk to figure out the split points.
        ScopedDbConnection conn( getShard().getConnString() );
        BSONObj result;
        BSONObjBuilder cmd;
        cmd.append( "splitVector" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKey().key() );
        cmd.append( "min" , getMin() );
        cmd.append( "max" , getMax() );
        cmd.append( "maxChunkSizeBytes" , chunkSize );
        cmd.append( "maxSplitPoints" , maxPoints );
        cmd.append( "maxChunkObjects" , maxObjs );
        BSONObj cmdObj = cmd.obj();

        if ( ! conn->runCommand( "admin" , cmdObj , result )){
            conn.done();
            ostringstream os;
            os << "splitVector command failed: " << result;
            uassert( 13345 , os.str() , 0 );
        }       

        BSONObjIterator it( result.getObjectField( "splitKeys" ) );
        while ( it.more() ){
            splitPoints.push_back( it.next().Obj().getOwned() );
        }
        conn.done();
    }

    ChunkPtr Chunk::singleSplit( bool force ){
        vector<BSONObj> splitPoint;

        // If splitting is not obligatory, we may return early if there are not enough data. 
        if ( ! force ) {
            vector<BSONObj> candidates;
            const int maxPoints = 2;
            const int maxObjs = 100000;
            pickSplitVector( candidates , getManager()->getCurrentDesiredChunkSize() , maxPoints , maxObjs );
            if ( candidates.size() <= 1 ) {
                // no split points means there isn't enough data to split on
                // 1 split point means we have between half the chunk size to full chunk size
                // so we shouldn't split
                log(1) << "chunk not full enough to trigger auto-split" << endl;
                return ChunkPtr();
            }

            splitPoint.push_back( candidates.front() );

        } else {
            // if forcing a split, use the chunk's median key
            BSONObj medianKey;
            pickMedianKey( medianKey );
            if ( ! medianKey.isEmpty() )
                splitPoint.push_back( medianKey );
        }

        // We assume that if the chunk being split is the first (or last) one on the collection, this chunk is
        // likely to see more insertions. Instead of splitting mid-chunk, we use the very first (or last) key
        // as a split point.
        if ( minIsInf() ) {
            splitPoint.clear();
            BSONObj key = _getExtremeKey( 1 );
            if ( ! key.isEmpty() ) {
                splitPoint.push_back( key );
            }

        } else if ( maxIsInf() ) {
            splitPoint.clear();
            BSONObj key = _getExtremeKey( -1 );
            if ( ! key.isEmpty() ) {
                splitPoint.push_back( key );
            }
        } 

        // Normally, we'd have a sound split point here if the chunk is not empty. It's also a good place to
        // sanity check.
        if ( splitPoint.empty() || _min == splitPoint.front() || _max == splitPoint.front() ) {
            log() << "want to split chunk, but can't find split point chunk " << toString()
                  << " got: " << ( splitPoint.empty() ? "<empty>" : splitPoint.front().toString() ) << endl;
            return ChunkPtr();
        }

        return multiSplit( splitPoint );
    }
    
    ChunkPtr Chunk::multiSplit( const vector<BSONObj>& m ) {
        const size_t maxSplitPoints = 256;

        uassert( 10165 , "can't split as shard doesn't have a manager" , _manager );
        uassert( 13332 , "need a split key to split chunk" , !m.empty() );
        uassert( 13333 , "can't split a chunk in that many parts", m.size() < maxSplitPoints );
        uassert( 13003 , "can't split a chunk with only one distinct value" , _min.woCompare(_max) ); 
        
        ScopedDbConnection conn( getShard().getConnString() );
        BSONObj result;
        BSONObjBuilder cmd;
        cmd.append( "splitChunk" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKey().key() );
        cmd.append( "min" , getMin() );
        cmd.append( "max" , getMax() );
        cmd.append( "splitKeys" , m );
        cmd.append( "shardId" , genID() );
        cmd.append( "configdb" , configServer.modelServer() ); 
        BSONObj cmdObj = cmd.obj();

        if ( ! conn->runCommand( "admin" , cmdObj , result )) {
            conn.done();

            // TODO decide if push up the error instead of asserting
            ostringstream os;
            os << "split chunk command failed: " << result;
            uassert( 13504 , os.str() , 0 );
        }

        conn.done();
        _manager->_reload();

        // The previous multisplit logic adjusted the boundaries of 'this' chunk. Any call to 'this' object hereafter
        // will see a different _max for the chunk.
        // TODO Untie this dependency since, for metadata purposes, the reload() above already fixed boundaries
        {
            rwlock lk( _manager->_lock , true );

            setMax(m[0].getOwned());
            DEV assert( shared_from_this() );
            _manager->_chunkMap[_max] = shared_from_this();
        }

        // return the second half, if a single split, or the first new chunk, if a multisplit.
        return _manager->findChunk( m[0] );
     }

    bool Chunk::moveAndCommit( const Shard& to , BSONObj& res ){
        uassert( 10167 ,  "can't move shard to its current location!" , getShard() != to );
        
        log() << "moving chunk ns: " << _manager->getns() << " moving ( " << toString() << ") " << _shard.toString() << " -> " << to.toString() << endl;
        
        Shard from = _shard;
        
        ScopedDbConnection fromconn( from);

        bool worked = fromconn->runCommand( "admin" ,
                                            BSON( "moveChunk" << _manager->getns() << 
                                                  "from" << from.getConnString() <<
                                                  "to" << to.getConnString() <<
                                                  "min" << _min << 
                                                  "max" << _max << 
                                                  "shardId" << genID() <<
                                                  "configdb" << configServer.modelServer()
                                                  ) ,
                                            res
                                            );
        
        fromconn.done();

        if ( worked ){
            _manager->_reload();
            return true;
        }

        return false;
    }
    
    bool Chunk::splitIfShould( long dataWritten ){
        LastError::Disabled d( lastError.get() );

        try {
            _dataWritten += dataWritten;        
            int splitThreshold = getManager()->getCurrentDesiredChunkSize();
            if ( minIsInf() || maxIsInf() ){
                splitThreshold = (int) ((double)splitThreshold * .9);
            }

            if ( _dataWritten < splitThreshold / 5 )
                return false;
        
            log(1) << "about to initiate autosplit: " << *this << " dataWritten: " << _dataWritten << endl;
            
            _dataWritten = 0; // reset so we check often enough
            
            ChunkPtr newShard = singleSplit( false /* does not force a split if not enough data */ );
            if ( newShard.get() == NULL ){
                // singleSplit would have issued a message if we got here
                return false;
            }

            log() << "autosplitted " << _manager->getns() << " shard: " << toString() 
                  << " on: " << newShard->getMax() << "(splitThreshold " << splitThreshold << ")" 
#ifdef _DEBUG
                  << " size: " << getPhysicalSize() // slow - but can be usefule when debugging
#endif
                  << endl;
        
            moveIfShould( newShard );
        
            return true;

        } catch ( std::exception& e ){
            // if the collection lock is taken (e.g. we're migrating), it is fine for the split to fail.
            log() << "autosplit failed: " << e.what() << endl;

            return false;
        }
    }

    bool Chunk::moveIfShould( ChunkPtr newChunk ){
        ChunkPtr toMove;
       
        if ( newChunk->countObjects(2) <= 1 ){
            toMove = newChunk;
        }
        else if ( this->countObjects(2) <= 1 ){
            DEV assert( shared_from_this() );
            toMove = shared_from_this();
        }
        else {
            // moving middle shards is handled by balancer
            return false;
        }

        assert( toMove );
        
        Shard newLocation = Shard::pick( getShard() );
        if ( getShard() == newLocation ){
            // if this is the best shard, then we shouldn't do anything (Shard::pick already logged our shard).
            log(1) << "recently split chunk: " << toString() << "already in the best shard" << endl;
            return 0;
        }

        log() << "moving chunk (auto): " << toMove->toString() << " to: " << newLocation.toString() << " #objects: " << toMove->countObjects() << endl;

        BSONObj res;
        massert( 10412 , 
                 str::stream() << "moveAndCommit failed: " << res , 
                 toMove->moveAndCommit( newLocation , res ) );
        
        return true;
    }

    long Chunk::getPhysicalSize() const{
        ScopedDbConnection conn( getShard().getConnString() );
        
        BSONObj result;
        uassert( 10169 ,  "datasize failed!" , conn->runCommand( "admin" , 
                                                                 BSON( "datasize" << _manager->getns()
                                                                       << "keyPattern" << _manager->getShardKey().key() 
                                                                       << "min" << getMin() 
                                                                       << "max" << getMax() 
                                                                       << "maxSize" << ( MaxChunkSize + 1 )
                                                                       << "estimate" << true
                                                                       ) , result ) );
        
        conn.done();
        return (long)result["size"].number();
    }

    int Chunk::countObjects(int maxCount) const { 
        static const BSONObj fields = BSON("_id" << 1 );

        ShardConnection conn( getShard() , _manager->getns() );
        
        // not using regular count as this is more flexible and supports $min/$max
        Query q = Query().minKey(_min).maxKey(_max);
        int n;
        {
            auto_ptr<DBClientCursor> c = conn->query(_manager->getns(), q, maxCount, 0, &fields);
            assert( c.get() );
            n = c->itcount();
        }        
        conn.done();
        return n;
    }

    void Chunk::appendShortVersion( const char * name , BSONObjBuilder& b ){
        BSONObjBuilder bb( b.subobjStart( name ) );
        bb.append( "min" , _min );
        bb.append( "max" , _max );
        bb.done();
    }
    
    bool Chunk::operator==( const Chunk& s ) const{
        return 
            _manager->getShardKey().compare( _min , s._min ) == 0 &&
            _manager->getShardKey().compare( _max , s._max ) == 0
            ;
    }

    void Chunk::serialize(BSONObjBuilder& to,ShardChunkVersion myLastMod){
        
        to.append( "_id" , genID( _manager->getns() , _min ) );

        if ( myLastMod.isSet() ){
            to.appendTimestamp( "lastmod" , myLastMod );
        }
        else if ( _lastmod.isSet() ){
            assert( _lastmod > 0 && _lastmod < 1000 );
            to.appendTimestamp( "lastmod" , _lastmod );
        }
        else {
            assert(0);
        }

        to << "ns" << _manager->getns();
        to << "min" << _min;
        to << "max" << _max;
        to << "shard" << _shard.getName();
    }

    string Chunk::genID( const string& ns , const BSONObj& o ) {
        StringBuilder buf( ns.size() + o.objsize() + 16 );
        buf << ns << "-";

        BSONObjIterator i(o);
        while ( i.more() ){
            BSONElement e = i.next();
            buf << e.fieldName() << "_" << e.toString(false, true);
        }

        return buf.str();
    }
    
    void Chunk::unserialize(const BSONObj& from){
        string ns = from.getStringField( "ns" );
        _shard.reset( from.getStringField( "shard" ) );

        _lastmod = from["lastmod"];
        assert( _lastmod > 0 );

        BSONElement e = from["minDotted"];

        if (e.eoo()){
            _min = from.getObjectField( "min" ).getOwned();
            _max = from.getObjectField( "max" ).getOwned();
        } 
        else { // TODO delete this case after giving people a chance to migrate
            _min = e.embeddedObject().getOwned();
            _max = from.getObjectField( "maxDotted" ).getOwned();
        }
        
        uassert( 10170 ,  "Chunk needs a ns" , ! ns.empty() );
        uassert( 13327 ,  "Chunk ns must match server ns" , ns == _manager->getns() );

        uassert( 10171 ,  "Chunk needs a server" , _shard.ok() );

        uassert( 10172 ,  "Chunk needs a min" , ! _min.isEmpty() );
        uassert( 10173 ,  "Chunk needs a max" , ! _max.isEmpty() );
    }

    string Chunk::modelServer() const {
        // TODO: this could move around?
        return configServer.modelServer();
    }
    
    ShardChunkVersion Chunk::getVersionOnConfigServer() const {
        ScopedDbConnection conn( modelServer() );
        BSONObj o = conn->findOne( ShardNS::chunk , BSON( "_id" << genID() ) );
        conn.done();
        return o["lastmod"];
    }

    string Chunk::toString() const {
        stringstream ss;
        ss << "ns:" << _manager->getns() << " at: " << _shard.toString() << " lastmod: " << _lastmod.toString() << " min: " << _min << " max: " << _max;
        return ss.str();
    }
    
    ShardKeyPattern Chunk::skey() const{
        return _manager->getShardKey();
    }

    // -------  ChunkManager --------

    AtomicUInt ChunkManager::NextSequenceNumber = 1;

    ChunkManager::ChunkManager( DBConfig * config , string ns , ShardKeyPattern pattern , bool unique ) : 
        _config( config ) , _ns( ns ) , 
        _key( pattern ) , _unique( unique ) , 
        _lock("rw:ChunkManager"), _nsLock( ConnectionString( configServer.modelServer() , ConnectionString::SYNC ) , ns )
    {
        _reload_inlock();  // will set _sequenceNumber
    }

    ChunkManager::~ChunkManager(){
        _chunkMap.clear();
        _chunkRanges.clear();
        _shards.clear();
    }
    
    void ChunkManager::_reload(){
        rwlock lk( _lock , true );
        _reload_inlock();
    }

    void ChunkManager::_reload_inlock(){
        int tries = 3;
        while (tries--){
            _chunkMap.clear();
            _chunkRanges.clear();
            _shards.clear();
            _load();

            if (_isValid()){
                _chunkRanges.reloadAll(_chunkMap);

                // The shard versioning mechanism hinges on keeping track of the number of times we reloaded ChunkManager's.
                // Increasing this number here will prompt checkShardVersion() to refresh the connection-level versions to
                // the most up to date value.
                _sequenceNumber = ++NextSequenceNumber; 

                return;
            }

            if (_chunkMap.size() < 10){ 
                _printChunks();
            }

            sleepmillis(10 * (3-tries));
        }

        msgasserted(13282, "Couldn't load a valid config for " + _ns + " after 3 attempts. Please try again.");
        
    }

    void ChunkManager::_load(){
        static Chunk temp(0);
        
        ScopedDbConnection conn( temp.modelServer() );

        // TODO really need the sort?
        auto_ptr<DBClientCursor> cursor = conn->query(temp.getNS(), QUERY("ns" << _ns).sort("lastmod",1), 0, 0, 0, 0,
                (DEBUG_BUILD ? 2 : 1000000)); // batch size. Try to induce potential race conditions in debug builds
        assert( cursor.get() );
        while ( cursor->more() ){
            BSONObj d = cursor->next();
            if ( d["isMaxMarker"].trueValue() ){
                continue;
            }
            
            ChunkPtr c( new Chunk( this ) );
            c->unserialize( d );

            _chunkMap[c->getMax()] = c;
            _shards.insert(c->getShard());

        }
        conn.done();
    }

    bool ChunkManager::_isValid() const {
#define ENSURE(x) do { if(!(x)) { log() << "ChunkManager::_isValid failed: " #x << endl; return false; } } while(0)

        if (_chunkMap.empty())
            return true;

        // Check endpoints
        ENSURE(allOfType(MinKey, _chunkMap.begin()->second->getMin()));
        ENSURE(allOfType(MaxKey, prior(_chunkMap.end())->second->getMax()));

        // Make sure there are no gaps or overlaps
        for (ChunkMap::const_iterator it=boost::next(_chunkMap.begin()), end=_chunkMap.end(); it != end; ++it){
            ChunkMap::const_iterator last = prior(it);

            if (!(it->second->getMin() == last->second->getMax())){
                PRINT(it->second->toString());
                PRINT(it->second->getMin());
                PRINT(last->second->getMax());
            }
            ENSURE(it->second->getMin() == last->second->getMax());
        }

        return true;

#undef ENSURE
    }

    void ChunkManager::_printChunks() const {
        for (ChunkMap::const_iterator it=_chunkMap.begin(), end=_chunkMap.end(); it != end; ++it) {
            log() << *it->second << endl;
        }
    }

    bool ChunkManager::hasShardKey( const BSONObj& obj ){
        return _key.hasShardKey( obj );
    }

    void ChunkManager::createFirstChunk(){
        ChunkPtr c( new Chunk(this, _key.globalMin(), _key.globalMax(), _config->getPrimary()) );
        c->setModified( true );
        
        _chunkMap[c->getMax()] = c;
        _chunkRanges.reloadAll(_chunkMap);
        
        _shards.insert(c->getShard());

        save_inlock( true );

        log() << "no chunks for:" << _ns << " so creating first: " << c->toString() << endl;
    }

    ChunkPtr ChunkManager::findChunk( const BSONObj & obj , bool retry ){
        BSONObj key = _key.extractKey(obj);
        
        {
            rwlock lk( _lock , false ); 
            
            BSONObj foo;
            ChunkPtr c;
            {
                ChunkMap::iterator it = _chunkMap.upper_bound(key);
                if (it != _chunkMap.end()){
                    foo = it->first;
                    c = it->second;
                }
            }
            
            if ( c ){
                if ( c->contains( obj ) )
                    return c;
                
                PRINT(foo);
                PRINT(*c);
                PRINT(key);
                
                _reload_inlock();
                massert(13141, "Chunk map pointed to incorrect chunk", false);
            }
        }

        if ( retry ){
            stringstream ss;
            ss << "couldn't find a chunk aftry retry which should be impossible extracted: " << key;
            throw UserException( 8070 , ss.str() );
        }
        
        log() << "ChunkManager: couldn't find chunk for: " << key << " going to retry" << endl;
        _reload_inlock();
        return findChunk( obj , true );
    }

    ChunkPtr ChunkManager::findChunkOnServer( const Shard& shard ) const {
        rwlock lk( _lock , false ); 
 
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            if ( c->getShard() == shard )
                return c;
        }

        return ChunkPtr();
    }

    void ChunkManager::getShardsForQuery( set<Shard>& shards , const BSONObj& query ){
        rwlock lk( _lock , false ); 
        DEV PRINT(query);

        //TODO look into FieldRangeSetOr
        FieldRangeOrSet fros(_ns.c_str(), query, false);

        const string special = fros.getSpecial();
        if (special == "2d") {
            BSONForEach(field, query){
                if (getGtLtOp(field) == BSONObj::opNEAR) {
                    uassert(13501, "use geoNear command rather than $near query", false);
                    // TODO: convert to geoNear rather than erroring out
                }
                // $within queries are fine
            }
        } else if (!special.empty()){
            uassert(13502, "unrecognized special query type: " + special, false);
        }

        do {
            boost::scoped_ptr<FieldRangeSet> frs (fros.topFrs());
            {
                // special case if most-significant field isn't in query
                FieldRange range = frs->range(_key.key().firstElement().fieldName());
                if ( !range.nontrivial() ){
                    DEV PRINT(range.nontrivial());
                    getAllShards(shards);
                    return;
                }
            }

            BoundList ranges = frs->indexBounds(_key.key(), 1);
            for (BoundList::const_iterator it=ranges.begin(), end=ranges.end(); it != end; ++it){
                BSONObj minObj = it->first.replaceFieldNames(_key.key());
                BSONObj maxObj = it->second.replaceFieldNames(_key.key());

                DEV PRINT(minObj);
                DEV PRINT(maxObj);

                ChunkRangeMap::const_iterator min, max;
                min = _chunkRanges.upper_bound(minObj);
                max = _chunkRanges.upper_bound(maxObj);
                
                massert( 13507 , str::stream() << "invalid chunk config minObj: " << minObj , min != _chunkRanges.ranges().end());

                // make max non-inclusive like end iterators
                if(max != _chunkRanges.ranges().end())
                    ++max;

                for (ChunkRangeMap::const_iterator it=min; it != max; ++it){
                    shards.insert(it->second->getShard());
                }

                // once we know we need to visit all shards no need to keep looping
                //if (shards.size() == _shards.size())
                    //return;
            }

            if (fros.moreOrClauses())
                fros.popOrClause();

        } while (fros.moreOrClauses());
    }

    void ChunkManager::getShardsForRange(set<Shard>& shards, const BSONObj& min, const BSONObj& max){
        uassert(13405, "min must have shard key", hasShardKey(min));
        uassert(13406, "max must have shard key", hasShardKey(max));

        ChunkRangeMap::const_iterator it = _chunkRanges.upper_bound(min);
        ChunkRangeMap::const_iterator end = _chunkRanges.lower_bound(max);

        for (; it!=end; ++ it){
            shards.insert(it->second->getShard());

            // once we know we need to visit all shards no need to keep looping
            if (shards.size() == _shards.size())
                break;
        }
    }

    void ChunkManager::getAllShards( set<Shard>& all ){
        rwlock lk( _lock , false ); 
        all.insert(_shards.begin(), _shards.end());
    }
    
    void ChunkManager::ensureIndex_inlock(){
        //TODO in parallel?
        for ( set<Shard>::const_iterator i=_shards.begin(); i!=_shards.end(); ++i ){
            ScopedDbConnection conn( i->getConnString() );
            conn->ensureIndex( getns() , getShardKey().key() , _unique , "" , false /* do not cache ensureIndex SERVER-1691 */ );
            conn.done();
        }
    }
    
    void ChunkManager::drop( ChunkManagerPtr me ){
        rwlock lk( _lock , true ); 

        configServer.logChange( "dropCollection.start" , _ns , BSONObj() );
        
        dist_lock_try dlk( &_nsLock  , "drop" );
        uassert( 13331 ,  "collection's metadata is undergoing changes. Please try again." , dlk.got() );
        
        uassert( 10174 ,  "config servers not all up" , configServer.allUp() );
        
        set<Shard> seen;
        
        log(1) << "ChunkManager::drop : " << _ns << endl;

        // lock all shards so no one can do a split/migrate
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            seen.insert( c->getShard() );
        }
        
        log(1) << "ChunkManager::drop : " << _ns << "\t all locked" << endl;        

        // wipe my meta-data
        _chunkMap.clear();
        _chunkRanges.clear();
        _shards.clear();
        
        // delete data from mongod
        for ( set<Shard>::iterator i=seen.begin(); i!=seen.end(); i++ ){
            ScopedDbConnection conn( *i );
            conn->dropCollection( _ns );
            conn.done();
        }
        
        log(1) << "ChunkManager::drop : " << _ns << "\t removed shard data" << endl;        

        // clean up database meta-data
        uassert( 10176 ,  "no sharding data?" , _config->removeSharding( _ns ) );
        
        // remove chunk data
        static Chunk temp(0);
        ScopedDbConnection conn( temp.modelServer() );
        conn->remove( temp.getNS() , BSON( "ns" << _ns ) );
        conn.done();
        log(1) << "ChunkManager::drop : " << _ns << "\t removed chunk data" << endl;                
        
        for ( set<Shard>::iterator i=seen.begin(); i!=seen.end(); i++ ){
            ScopedDbConnection conn( *i );
            BSONObj res;
            if ( ! setShardVersion( conn.conn() , _ns , 0 , true , res ) )
                throw UserException( 8071 , str::stream() << "cleaning up after drop failed: " << res );
            conn.done();
        }

        log(1) << "ChunkManager::drop : " << _ns << "\t DONE" << endl;        
        configServer.logChange( "dropCollection" , _ns , BSONObj() );
    }
    
    void ChunkManager::save( bool major ){
        rwlock lk( _lock , true ); 
        save_inlock( major );
    }
    
    void ChunkManager::save_inlock( bool major ){
        // we do not update update the chunk manager on the mongos side any more
        // the only exception case should be first chunk creation
        assert( _chunkMap.size() == 1 );

        ShardChunkVersion version = getVersion_inlock();
        assert( version > 0 || _chunkMap.size() <= 1 );
        ShardChunkVersion nextChunkVersion = version;
        nextChunkVersion.inc( major );

        vector<ChunkPtr> toFix;
        vector<ShardChunkVersion> newVersions;
        
        // Instead of upating the 'chunks' collection directly, we use the 'applyOps' command. It allows us 
        // (a) to serialize the changes to that collection and (b) to only actually perform the update if this
        // ChunkManager has the proper ShardChunkVersion.
        BSONObjBuilder cmdBuilder;
        BSONArrayBuilder updates( cmdBuilder.subarrayStart( "applyOps" ) );
        
        int numOps = 0;
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            if ( ! c->getModified() )
                continue;

            numOps++;
            _sequenceNumber = ++NextSequenceNumber;

            ShardChunkVersion myVersion = nextChunkVersion;
            nextChunkVersion.incMinor();
            toFix.push_back( c );
            newVersions.push_back( myVersion );

            // build an update operation against the chunks collection of the config database with 
            // upsert true
            BSONObjBuilder op;
            op.append( "op" , "u" );
            op.appendBool( "b" , true );
            op.append( "ns" , ShardNS::chunk );

            // add the modified (new) chunk infomation as the update object
            BSONObjBuilder n( op.subobjStart( "o" ) );
            c->serialize( n , myVersion ); // n will get full 'c' info plus version
            n.done();

            // add the chunk's _id as the query part of the update statement
            BSONObjBuilder q( op.subobjStart( "o2" ) );
            q.append( "_id" , c->genID() );
            q.done();

            updates.append( op.obj() );
        }
        
        if ( numOps == 0 )
            return;
        
        updates.done();
        
        if ( version > 0 || _chunkMap.size() > 1 ){
            BSONArrayBuilder temp( cmdBuilder.subarrayStart( "preCondition" ) );
            BSONObjBuilder b;
            b.append( "ns" , ShardNS::chunk );
            b.append( "q" , BSON( "query" << BSON( "ns" << _ns ) << "orderby" << BSON( "lastmod" << -1 ) ) );
            {
                BSONObjBuilder bb( b.subobjStart( "res" ) );
                bb.appendTimestamp( "lastmod" , version );
                bb.done();
            }
            temp.append( b.obj() );
            temp.done();
        }
        // TODO preCondition for initial chunk or starting collection 

        BSONObj cmd = cmdBuilder.obj();
        
        log(7) << "ChunkManager::save update: " << cmd << endl;
        
        ScopedDbConnection conn( Chunk(0).modelServer() );
        BSONObj res;
        bool ok = conn->runCommand( "config" , cmd , res );
        conn.done();

        if ( ! ok ){
            stringstream ss;
            ss << "saving chunks failed.  cmd: " << cmd << " result: " << res;
            log( LL_ERROR ) << ss.str() << endl;
            msgasserted( 13327 , ss.str() );
        }

        // instead of reloading, adjust ShardChunkVersion for the chunks that were updated in the configdb
        for ( unsigned i=0; i<toFix.size(); i++ ){
            toFix[i]->setLastmod( newVersions[i] );
            toFix[i]->setModified( false );
        }

        massert( 10417 ,  "how did version get smalled" , getVersion_inlock() >= version );
        
        ensureIndex_inlock(); // TODO: this is too aggressive - but not really sooo bad
    }
    
    void ChunkManager::maybeChunkCollection() {
        uassert( 13346 , "can't pre-split already splitted collection" , (_chunkMap.size() == 1) );

        ChunkPtr soleChunk = _chunkMap.begin()->second;
        vector<BSONObj> splitPoints;
        soleChunk->pickSplitVector( splitPoints , Chunk::MaxChunkSize );
        if ( splitPoints.empty() ){
            log(1) << "not enough data to warrant chunking " << getns() << endl;
            return;
        }

        soleChunk->multiSplit( splitPoints );
    }

    ShardChunkVersion ChunkManager::getVersionOnConfigServer() const {
        static Chunk temp(0);
        
        ScopedDbConnection conn( temp.modelServer() );
        
        auto_ptr<DBClientCursor> cursor = conn->query(temp.getNS(), QUERY("ns" << _ns).sort("lastmod",1), 1 );
        assert( cursor.get() );
        BSONObj o;
        if ( cursor->more() )
            o = cursor->next();
        conn.done();
             
        return o["lastmod"];
    }

    ShardChunkVersion ChunkManager::getVersion( const Shard& shard ) const{
        rwlock lk( _lock , false ); 
        // TODO: cache or something?
        
        ShardChunkVersion max = 0;

        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            DEV assert( c );
            if ( c->getShard() != shard )
                continue;
            if ( c->getLastmod() > max )
                max = c->getLastmod();
        }        
        return max;
    }

    ShardChunkVersion ChunkManager::getVersion() const{
        rwlock lk( _lock , false ); 
        return getVersion_inlock();
    }
    
    ShardChunkVersion ChunkManager::getVersion_inlock() const{
        ShardChunkVersion max = 0;
        
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            ChunkPtr c = i->second;
            if ( c->getLastmod() > max )
                max = c->getLastmod();
        }        

        return max;
    }

    string ChunkManager::toString() const {
        rwlock lk( _lock , false );         

        stringstream ss;
        ss << "ChunkManager: " << _ns << " key:" << _key.toString() << '\n';
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ){
            const ChunkPtr c = i->second;
            ss << "\t" << c->toString() << '\n';
        }
        return ss.str();
    }

    void ChunkManager::_migrationNotification(Chunk* c){
        _chunkRanges.reloadRange(_chunkMap, c->getMin(), c->getMax());
        _shards.insert(c->getShard());
    }

    
    void ChunkRangeManager::assertValid() const{
        if (_ranges.empty())
            return;

        try {
            // No Nulls
            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it){
                assert(it->second);
            }
            
            // Check endpoints
            assert(allOfType(MinKey, _ranges.begin()->second->getMin()));
            assert(allOfType(MaxKey, prior(_ranges.end())->second->getMax()));

            // Make sure there are no gaps or overlaps
            for (ChunkRangeMap::const_iterator it=boost::next(_ranges.begin()), end=_ranges.end(); it != end; ++it){
                ChunkRangeMap::const_iterator last = prior(it);
                assert(it->second->getMin() == last->second->getMax());
            }

            // Check Map keys
            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it){
                assert(it->first == it->second->getMax());
            }

            // Make sure we match the original chunks
            const ChunkMap chunks = _ranges.begin()->second->getManager()->_chunkMap;
            for ( ChunkMap::const_iterator i=chunks.begin(); i!=chunks.end(); ++i ){
                const ChunkPtr chunk = i->second;

                ChunkRangeMap::const_iterator min = _ranges.upper_bound(chunk->getMin());
                ChunkRangeMap::const_iterator max = _ranges.lower_bound(chunk->getMax());

                assert(min != _ranges.end());
                assert(max != _ranges.end());
                assert(min == max);
                assert(min->second->getShard() == chunk->getShard());
                assert(min->second->contains( chunk->getMin() ));
                assert(min->second->contains( chunk->getMax() ) || (min->second->getMax() == chunk->getMax()));
            }
            
        } catch (...) {
            log( LL_ERROR ) << "\t invalid ChunkRangeMap! printing ranges:" << endl;

            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it)
                cout << it->first << ": " << *it->second << endl;

            throw;
        }
    }

    void ChunkRangeManager::reloadRange(const ChunkMap& chunks, const BSONObj& min, const BSONObj& max){
        if (_ranges.empty()){
            reloadAll(chunks);
            return;
        }
        
        ChunkRangeMap::iterator low  = _ranges.upper_bound(min);
        ChunkRangeMap::iterator high = _ranges.lower_bound(max);
        
        assert(low != _ranges.end());
        assert(high != _ranges.end());
        assert(low->second);
        assert(high->second);

        ChunkMap::const_iterator begin = chunks.upper_bound(low->second->getMin());
        ChunkMap::const_iterator end   = chunks.lower_bound(high->second->getMax());

        assert(begin != chunks.end());
        assert(end != chunks.end());

        // C++ end iterators are one-past-last
        ++high;
        ++end;

        // update ranges
        _ranges.erase(low, high); // invalidates low
        _insertRange(begin, end);

        assert(!_ranges.empty());
        DEV assertValid();

        // merge low-end if possible
        low = _ranges.upper_bound(min);
        assert(low != _ranges.end());
        if (low != _ranges.begin()){
            shared_ptr<ChunkRange> a = prior(low)->second;
            shared_ptr<ChunkRange> b = low->second;
            if (a->getShard() == b->getShard()){
                shared_ptr<ChunkRange> cr (new ChunkRange(*a, *b));
                _ranges.erase(prior(low));
                _ranges.erase(low); // invalidates low
                _ranges[cr->getMax()] = cr;
            }
        }

        DEV assertValid();

        // merge high-end if possible
        high = _ranges.lower_bound(max);
        if (high != prior(_ranges.end())){
            shared_ptr<ChunkRange> a = high->second;
            shared_ptr<ChunkRange> b = boost::next(high)->second;
            if (a->getShard() == b->getShard()){
                shared_ptr<ChunkRange> cr (new ChunkRange(*a, *b));
                _ranges.erase(boost::next(high));
                _ranges.erase(high); //invalidates high
                _ranges[cr->getMax()] = cr;
            }
        }

        DEV assertValid();
    }

    void ChunkRangeManager::reloadAll(const ChunkMap& chunks){
        _ranges.clear();
        _insertRange(chunks.begin(), chunks.end());

        DEV assertValid();
    }

    void ChunkRangeManager::_insertRange(ChunkMap::const_iterator begin, const ChunkMap::const_iterator end){
        while (begin != end){
            ChunkMap::const_iterator first = begin;
            Shard shard = first->second->getShard();
            while (begin != end && (begin->second->getShard() == shard))
                ++begin;

            shared_ptr<ChunkRange> cr (new ChunkRange(first, begin));
            _ranges[cr->getMax()] = cr;
        }
    }

    int ChunkManager::getCurrentDesiredChunkSize() const {
        // split faster in early chunks helps spread out an initial load better
        const int minChunkSize = 1 << 20;  // 1 MBytes

        int splitThreshold = Chunk::MaxChunkSize;
        
        int nc = numChunks();
        
        if ( nc < 10 ){
            splitThreshold = max( splitThreshold / 4 , minChunkSize );
        } 
        else if ( nc < 20 ){
            splitThreshold = max( splitThreshold / 2 , minChunkSize );
        }
        
        return splitThreshold;
    }
    
    class ChunkObjUnitTest : public UnitTest {
    public:
        void runShard(){
            ChunkPtr c;
            assert( ! c );
            c.reset( new Chunk( 0 ) );
            assert( c );
        }
        
        void runShardChunkVersion(){
            vector<ShardChunkVersion> all;
            all.push_back( ShardChunkVersion(1,1) );
            all.push_back( ShardChunkVersion(1,2) );
            all.push_back( ShardChunkVersion(2,1) );
            all.push_back( ShardChunkVersion(2,2) );
            
            for ( unsigned i=0; i<all.size(); i++ ){
                for ( unsigned j=i+1; j<all.size(); j++ ){
                    assert( all[i] < all[j] );
                }
            }

        }

        void run(){
            runShard();
            runShardChunkVersion();
            log(1) << "shardObjTest passed" << endl;
        }
    } shardObjTest;


    // ----- to be removed ---
    extern OID serverID;
    bool setShardVersion( DBClientBase & conn , const string& ns , ShardChunkVersion version , bool authoritative , BSONObj& result ){
        BSONObjBuilder cmdBuilder;
        cmdBuilder.append( "setShardVersion" , ns.c_str() );
        cmdBuilder.append( "configdb" , configServer.modelServer() );
        cmdBuilder.appendTimestamp( "version" , version.toLong() );
        cmdBuilder.appendOID( "serverID" , &serverID );
        if ( authoritative )
            cmdBuilder.appendBool( "authoritative" , 1 );

        Shard s = Shard::make( conn.getServerAddress() );
        cmdBuilder.append( "shard" , s.getName() );
        cmdBuilder.append( "shardHost" , s.getConnString() );
        BSONObj cmd = cmdBuilder.obj();
        
        log(1) << "    setShardVersion  " << s.getName() << " " << conn.getServerAddress() << "  " << ns << "  " << cmd << " " << &conn << endl;
        
        return conn.runCommand( "admin" , cmd , result );
    }

} // namespace mongo
