// @file d_logic.cpp

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


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "pch.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"

#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"
#include "d_writeback.h"

using namespace std;

namespace mongo {

    bool _handlePossibleShardedMessage( Message &m, DbResponse* dbresponse ) {
        DEV verify( shardingState.enabled() );

        int op = m.operation();
        if ( op < 2000
                || op >= 3000
                || op == dbGetMore  // cursors are weird
           )
            return false;

        DbMessage d(m);
        const char *ns = d.getns();
        string errmsg;
        // We don't care about the version here, since we're returning it later in the writeback
        ConfigVersion received, wanted;
        if ( shardVersionOk( ns , errmsg, received, wanted ) ) {
            return false;
        }

        bool getsAResponse = doesOpGetAResponse( op );

        LOG(1) << "connection sharding metadata does not match for collection " << ns
               << ", will retry (wanted : " << wanted << ", received : " << received << ")"
               << ( getsAResponse ? "" : " (queuing writeback)" ) << endl;

        if( getsAResponse ){
            verify( dbresponse );
            BufBuilder b( 32768 );
            b.skip( sizeof( QueryResult ) );
            {
                BSONObjBuilder bob;

                bob.append( "$err", errmsg );
                bob.append( "ns", ns );
                wanted.addToBSON( bob, "vWanted" );
                received.addToBSON( bob, "vReceived" );

                BSONObj obj = bob.obj();

                b.appendBuf( obj.objdata() , obj.objsize() );
            }

            QueryResult *qr = (QueryResult*)b.buf();
            qr->_resultFlags() = ResultFlag_ErrSet | ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation( opReply );
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            b.decouple();

            Message * resp = new Message();
            resp->setData( qr , true );

            dbresponse->response = resp;
            dbresponse->responseTo = m.header()->id;
            return true;
        }
        
        uassert( 9517 , "writeback" , ( d.reservedField() & Reserved_FromWriteback ) == 0 );

        OID writebackID;
        writebackID.initSequential();
        lastError.getSafe()->writeback( writebackID );

        const OID& clientID = ShardedConnectionInfo::get(false)->getID();
        massert( 10422 ,  "write with bad shard config and no server id!" , clientID.isSet() );

        LOG(1) << "writeback queued for " << m.toString() << endl;

        BSONObjBuilder b;
        b.appendBool( "writeBack" , true );
        b.append( "ns" , ns );
        b.append( "id" , writebackID );
        b.append( "connectionId" , cc().getConnectionId() );
        b.append( "instanceIdent" , prettyHostName() );
        wanted.addToBSON( b );
        received.addToBSON( b, "yourVersion" );

        b.appendBinData( "msg" , m.header()->len , bdtCustom , (char*)(m.singleData()) );
        LOG(2) << "writing back msg with len: " << m.header()->len << " op: " << m.operation() << endl;
        writeBackManager.queueWriteBack( clientID.str() , b.obj() );

        return true;
    }

}
