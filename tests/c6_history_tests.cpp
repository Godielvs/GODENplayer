#include "../src/component/collection/collection_audio_history.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
using Data=std::array<float,4>;
using History=nekorox::component::c6::AudioRowHistory<Data>;
using Identity=nekorox::component::c6::AudioIdentity;
using Mode=nekorox::component::c6::HistoryReadMode;
using Reader=nekorox::component::collection::AudioHistoryReader;
using ReaderHistory=nekorox::component::collection::RetainedAudioRows;
using Receipt=nekorox::component::collection::HistoryApplyReceipt;

void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}

template<class H>void publish(H&history,std::uint64_t first,std::uint64_t last,
    Identity identity={1,101}){
    for(auto n=first;n<=last;++n)history.publish(identity.serial,
        identity.recording,n,false,{float(n),0,0,0});
}

void testFullRestoreIsOldestFirst(){
    History history;History::Batch batch;publish(history,1,512);
    std::uint64_t cursor=0,fence=0;std::size_t restored=0;
    for(unsigned read=0;read<8;++read){
        check(history.read({1,101},cursor,Mode::restore,fence,batch),
            "C6 history restore unexpectedly contended");
        check(!batch.oldIdentity&&batch.count==64,
            "C6 history restore did not return a bounded full page");
        if(!fence)fence=batch.restoreFence;
        check(fence==512,"C6 history restore fence moved");
        check(batch.rows[0].pixels[0]==float(restored+1),
            "C6 history restore skipped its oldest retained row");
        restored+=batch.count;cursor=batch.rows[batch.count-1].publication;
        check(batch.complete==(read==7),"C6 history completed at the wrong page");
    }
    check(restored==512&&cursor==512,"C6 history did not restore all 512 rows");
}

void testRestoreFenceDoesNotChaseNewRows(){
    History history;History::Batch batch;publish(history,1,512);
    check(history.read({1,101},0,Mode::restore,0,batch),"Initial restore read failed");
    const auto fence=batch.restoreFence;auto cursor=batch.rows[batch.count-1].publication;
    publish(history,513,540);
    std::size_t restored=batch.count;
    while(cursor<fence){
        check(history.read({1,101},cursor,Mode::restore,fence,batch),
            "Fenced restore read failed");
        check(batch.restoreFence==fence,"New publications extended a frozen restore");
        check(batch.count!=0,"Fenced restore made no progress");
        restored+=batch.count;cursor=batch.rows[batch.count-1].publication;
    }
    check(restored==512&&cursor==512&&batch.complete,
        "Fenced restore did not finish the original 512-row snapshot");
    check(history.read({1,101},cursor,Mode::incremental,0,batch)&&batch.count==28,
        "Incremental read did not expose rows published after the restore fence");
}

void testIdentityIsAnInput(){
    History history;History::Batch batch;publish(history,1,12,{1,101});
    check(history.read({2,202},0,Mode::restore,0,batch),"Identity probe failed");
    check(batch.oldIdentity&&batch.count==0,
        "A cold reader adopted retained rows from the wrong recording");
    history.publish(2,202,13,false,{13,0,0,0});
    check(history.read({2,202},0,Mode::restore,0,batch),"New identity read failed");
    check(!batch.oldIdentity&&batch.count==1&&batch.recording==202&&
        batch.rows[0].pixels[0]==13,"New recording did not start cleanly");
}

void testReadersAreIndependent(){
    History history;History::Batch a,b;publish(history,1,100);
    check(history.read({1,101},0,Mode::restore,0,a)&&a.count==64,
        "First history reader failed");
    check(history.read({1,101},0,Mode::restore,0,b)&&b.count==64,
        "Second history reader inherited the first cursor");
    check(a.rows[0].publication==b.rows[0].publication,
        "History readers did not begin independently");
}

void testOverwriteIsExplicit(){
    using Small=nekorox::component::c6::AudioRowHistory<Data,8,2>;
    Small history;Small::Batch batch;
    for(std::uint64_t n=1;n<=8;++n)history.publish(1,101,n,false,{float(n),0,0,0});
    check(history.read({1,101},0,Mode::restore,0,batch)&&batch.count==2,
        "Small restore did not begin");
    const auto fence=batch.restoreFence;auto cursor=batch.rows[1].publication;
    for(std::uint64_t n=9;n<=12;++n)history.publish(1,101,n,false,{float(n),0,0,0});
    check(history.read({1,101},cursor,Mode::restore,fence,batch),
        "Overwrite restore read failed");
    check(batch.skippedObservations==2&&batch.rows[0].publication==5,
        "Overwritten retained rows were not reported explicitly");
}

void publishRows(ReaderHistory&history,std::uint64_t first,std::uint64_t last){
    for(auto n=first;n<=last;++n){decltype(nekorox::component::collection::AudioFrame{}.row) row{};
        row[0].x=float(n);history.publish(1,101,n,false,row);}
}

Receipt applied(const Reader&reader,const ReaderHistory::Batch&batch,
    std::uint64_t resourceEpoch,bool presentAccepted){
    (void)presentAccepted;Receipt receipt;receipt.identity=reader.identity();
    receipt.readerGeneration=reader.generation();receipt.resourceEpoch=resourceEpoch;
    receipt.throughPublication=batch.count?batch.rows[batch.count-1].publication:batch.restoreFence;
    receipt.skippedObservations=batch.skippedObservations;receipt.handled=true;
    receipt.restoreComplete=batch.complete;return receipt;
}

void testCursorCommitsOnlyAfterUpload(){
    auto history=std::make_unique<ReaderHistory>();publishRows(*history,1,512);Reader reader;reader.reset({1,101});
    const auto*first=reader.stage(*history);check(first&&first->count==64,
        "Transactional history reader did not stage its first page");
    check(reader.cursor()==0&&reader.plannedThrough()==64,
        "Staging advanced the applied history cursor");
    // Bitmap-only re-presentation: no upload receipt, so the exact page remains.
    const auto*represented=reader.stage(*history);check(represented==first&&
        represented->rows[0].publication==1&&reader.cursor()==0,
        "Bitmap re-presentation consumed an unapplied page");
    // A submitted texture upload is consumed even when display Present rejects.
    const auto receipt=applied(reader,*represented,7,false);
    check(reader.commit(receipt)&&reader.cursor()==64,
        "Rejected Present incorrectly blocked an applied texture upload");
    check(!reader.commit(receipt),"One upload receipt was accepted twice");
    const auto*second=reader.stage(*history);check(second&&second->rows[0].publication==65,
        "Reader did not advance exactly once after texture upload");
}

void testStaleGenerationAndResourceReceiptRejected(){
    auto history=std::make_unique<ReaderHistory>();publishRows(*history,1,128);Reader reader;reader.reset({1,101});
    const auto*page=reader.stage(*history);check(page!=nullptr,"Could not stage stale-generation fixture");
    const auto stale=applied(reader,*page,11,true);reader.reset({1,101});
    check(!reader.commit(stale)&&reader.cursor()==0,
        "Receipt from an old device/configuration generation was accepted");
    page=reader.stage(*history);check(page!=nullptr,"Could not restage after generation reset");
    check(reader.commit(applied(reader,*page,12,true)),"Fresh resource epoch was rejected");
    page=reader.stage(*history);check(page!=nullptr,"Could not stage resource-epoch fixture");
    check(!reader.commit(applied(reader,*page,13,true))&&reader.cursor()==64,
        "Receipt from a different texture resource epoch was accepted");
}

void testExpiredEmptyPageIsHandledOnce(){
    using Small=nekorox::component::c6::AudioRowHistory<Data,8,2>;
    Small small;Small::Batch smallBatch;publish(small,1,8);
    check(small.read({1,101},0,Mode::restore,0,smallBatch)&&smallBatch.count==2,
        "Exact expiration fixture did not start");
    const auto smallCursor=smallBatch.rows[1].publication,smallFence=smallBatch.restoreFence;
    publish(small,9,20);check(small.read({1,101},smallCursor,Mode::restore,smallFence,smallBatch),
        "Exact expiration fixture read failed");
    check(smallBatch.count==0&&smallBatch.complete&&smallBatch.skippedObservations==6,
        "Expired restore did not count the exact missing range inside its fence");

    auto history=std::make_unique<ReaderHistory>();publishRows(*history,1,512);Reader reader;reader.reset({1,101});
    auto*page=reader.stage(*history);check(page&&reader.commit(applied(reader,*page,21,true)),
        "Production reader fixture did not apply its first page");
    publishRows(*history,513,1024);page=reader.stage(*history);
    check(page&&page->count==0&&page->complete&&page->skippedObservations==448,
        "Production reader did not stage its empty expired restore page");
    check(reader.commit(applied(reader,*page,21,false))&&reader.cursor()==512&&
        !reader.restoring(),"Empty expiration did not confirm the frozen fence");
    page=reader.stage(*history);check(page&&page->rows[0].publication==513&&
        page->skippedObservations==0,
        "Post-fence incremental read double-counted expired observations");
}
}

void runC6HistoryTests(){
    testFullRestoreIsOldestFirst();
    testRestoreFenceDoesNotChaseNewRows();
    testIdentityIsAnInput();
    testReadersAreIndependent();
    testOverwriteIsExplicit();
    testCursorCommitsOnlyAfterUpload();
    testStaleGenerationAndResourceReceiptRejected();
    testExpiredEmptyPageIsHandledOnce();
}

#ifdef GODEN_C6_HISTORY_STANDALONE
int main(){
    try{runC6HistoryTests();std::cout<<"C6 retained history contracts: PASS\n";return 0;}
    catch(const std::exception&error){std::cerr<<"C6 retained history contracts: FAIL: "<<error.what()<<'\n';return 1;}
}
#endif
