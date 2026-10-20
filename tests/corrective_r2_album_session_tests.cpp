#include "nekorox/album_session.h"

#include <algorithm>
#include <stdexcept>

void runCorrectiveR2AlbumSessionTests()
{
    using namespace nekorox;

    const AlbumContinuationToken eof{10,20,30,40};
    if (!albumContinuationAllowed(eof,eof,true,true,false,false))
        throw std::runtime_error("owned EOF continuation rejected");
    for (int invalid=0;invalid<8;++invalid) {
        auto now=eof; if(invalid==0)++now.intent;if(invalid==1)++now.request;
        if(invalid==2)++now.session;if(invalid==3)++now.composition;
        if(albumContinuationAllowed(eof,now,invalid!=4,invalid!=5,invalid==6,invalid==7))
            throw std::runtime_error("stale or unowned EOF continuation accepted");
    }

    AlbumPlaylistPreparationLease lease;
    if (!lease.needsPreparation(10, 4)) {
        throw std::runtime_error("C02 initial composition was not prepared");
    }
    lease.prepared(10, 4);
    for (int command = 0; command < 20; ++command) {
        if (lease.needsPreparation(10, 4)) {
            throw std::runtime_error(
                "C02 local command rebuilt an unchanged owned playlist");
        }
    }
    lease.externallyModified();
    if (!lease.needsPreparation(10, 4)) {
        throw std::runtime_error(
            "C02 external playlist edit did not revoke composition lease");
    }
    lease.prepared(10, 5);
    if (lease.needsPreparation(10, 5)) {
        throw std::runtime_error("C02 renewed composition lease stayed dirty");
    }

    AlbumSequencePolicy sequential;
    const auto natural = makeAlbumPermutation(5, 2, sequential);
    if (natural != std::vector<std::size_t>({0, 1, 2, 3, 4})) {
        throw std::runtime_error("C02 non-shuffle order changed track order");
    }
    sequential.repeat = AlbumRepeatMode::album;
    if (resolveAlbumPolicyIndex(natural, 4, 1, sequential.repeat) != 0 ||
        resolveAlbumPolicyIndex(natural, 0, -1, sequential.repeat) != 4) {
        throw std::runtime_error("C02 repeat-album did not wrap locally");
    }
    if (resolveAlbumPolicyIndex(
            natural, 3, 1, AlbumRepeatMode::track) != 3) {
        throw std::runtime_error("C02 repeat-track did not retain track");
    }
    if (resolveAlbumPolicyIndex(
            natural, 4, 1, AlbumRepeatMode::off).has_value()) {
        throw std::runtime_error("C02 repeat-off crossed the album boundary");
    }

    AlbumSequencePolicy shuffled;
    shuffled.shuffle = true;
    shuffled.seed = 0x12345678U;
    const auto first = makeAlbumPermutation(32, 17, shuffled);
    const auto second = makeAlbumPermutation(32, 17, shuffled);
    auto sorted = first;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        if (sorted[index] != index) {
            throw std::runtime_error("C02 shuffle permutation lost a track");
        }
    }
    if (first != second || first.front() != 17) {
        throw std::runtime_error(
            "C02 shuffle was not stable or did not preserve current track");
    }

    PlaybackPrecedenceInput precedence;
    precedence.keepPlaying = true;
    precedence.repeatAlbum = true;
    precedence.manualQueue = true;
    if (resolvePlaybackPrecedence(precedence) !=
        PlaybackPrecedence::manualQueue) {
        throw std::runtime_error("C02 manual queue did not win EOF routing");
    }
    precedence.userStop = true;
    if (resolvePlaybackPrecedence(precedence) != PlaybackPrecedence::userStop) {
        throw std::runtime_error("C02 explicit stop did not win EOF routing");
    }
}
