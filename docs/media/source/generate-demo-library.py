"""Original demo catalogue: unequal counts, sustained bass, original spoken words.

Generated images are copied unchanged. Original v1 procedural covers are retained
as secondary examples. Nothing in this script modifies a personal profile.
"""
from pathlib import Path
import hashlib, importlib.util, json, math, shutil, subprocess, sys, wave
import numpy as np
from scipy.signal import butter, sosfilt

ROOT = Path(__file__).resolve().parent
import procedural_covers as oldmedia
LIB = ROOT / 'demo-library'
FFMPEG = shutil.which('ffmpeg') or 'ffmpeg'
SR = 48000
ALBUMS = [
    ('Tidal Signal', 'Low Tide Club', 'Electronic', 7, 28, 'tidal-signal.png',
     ['Quiet Current', 'Blue Hour', 'Harbour Lights', 'Undertow', 'Salt Air', 'Between Stations', 'Return Signal']),
    ('After the Rain', 'Glass Orchard', 'Trip-Hop', 5, 33, 'after-the-rain-v2.png',
     ['Silver Line', 'Violet Morning', 'The Long Way', 'Glasshouse', 'Room to Grow']),
    ('Soft Current', 'Glass Orchard', 'Ambient', 3, 31, None,
     ['First Light', 'Suspended Lines', 'Undertow']),
    ('Amber Circuit', 'Low Tide Club', 'Downtempo', 4, 29, None,
     ['Copper Hour', 'Afterimage', 'Warm Signal', 'Last Connection']),
    ('Night Geometry', 'Sable Coast', 'Synthwave', 8, 26, None,
     ['Nightfall', 'Glass Arc', 'Quiet Motion', 'Parallel Lights', 'North Window', 'City Limits', 'Overpass', 'Dawn Sequence']),
    ('Contour Studies', 'Field Atlas', 'Ambient', 5, 33, None,
     ['Drift Pattern', 'Paper Horizon', 'Slow Orbit', 'Woodland', 'The Distance']),
    ('Phase Garden', 'Mira Vale', 'Neo-Soul', 6, 28, None,
     ['Signal Bloom', 'Open Form', 'Field Notes', 'Sunroom', 'Small Hours', 'Home Again']),
    ('Prism Notes', 'Sable Coast', 'Electronic', 3, 30, None,
     ['Rose Window', 'Chromatic Air', 'Returning']),
    ('Quiet Rooms', 'Northbound', 'Piano', 4, 36, None,
     ['Window Seat', 'Late Afternoon', 'Rain Study', 'The Last Room']),
]

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def synth(index, variation, duration, path):
    title, artist, genre, _, bass_root, _, _ = ALBUMS[index]
    beat = 60 / (96 if index != 1 else 88)
    rng = np.random.default_rng(250140 + 97*index + variation)
    count = int(SR * duration)
    audio = np.zeros((count, 2), np.float32)
    def add(sig, when, gain, pan=0):
        start = int(when*SR)
        if start >= count: return
        if start < 0: raise ValueError('negative placement')
        sig = np.asarray(sig, np.float32)[:count-start] * gain
        audio[start:start+len(sig), 0] += sig*math.sqrt((1-pan)/2)
        audio[start:start+len(sig), 1] += sig*math.sqrt((1+pan)/2)
    def oscillator(midi, length, kind):
        t = np.arange(int(length*SR), dtype=np.float32)/SR
        f = 440*2**((midi-69)/12)
        s = np.sin(2*np.pi*f*t)
        if kind == 'bass':
            # Stable phase/fundamental, rounded attack and a genuine long sustain.
            s += .24*np.sin(4*np.pi*f*t) + .055*np.sin(6*np.pi*f*t)
            env = np.minimum(t/.028, 1)*np.minimum((length-t)/.12, 1)
        elif kind == 'pad':
            s += .22*np.sin(2*np.pi*(2*f+.16)*t)
            env = np.minimum(t/.35, 1)*np.minimum((length-t)/.8, 1)
        else:
            s += .22*np.sin(4*np.pi*f*t) + .07*np.sin(6*np.pi*f*t)
            env = (1-np.exp(-t*160))*np.exp(-t*(2.2 if genre=='Piano' else 4.2))
            env *= np.minimum((length-t)/.08, 1)
        return s*np.clip(env,0,1)
    progression = [0, -5, -3, -7]
    for bar in range(math.ceil(duration/(8*beat))):
        start = bar*8*beat
        base = bass_root + progression[(bar+variation)%4]
        if genre != 'Piano':
            # Four-beat notes make the locked waveform easy to see, with space
            # for the kick; pitch changes stay tied to musical phrases.
            for off in (0,4):
                add(oscillator(base,3.85*beat,'bass'),start+off*beat,.34)
        for j,n in enumerate((0,3,7,10)):
            add(oscillator(base+36+n,8*beat+.5,'pad'),start,.033,(-.7,-.2,.2,.7)[j])
    for step in range(math.ceil(duration/(beat*.5))):
        at = step*beat*.5
        base = bass_root + progression[(int(at/(8*beat))+variation)%4]
        note = base+48+(0,7,10,14,7,3,12,10)[(step+variation)%8]
        gain = .06 if genre=='Piano' else .032
        sig = oscillator(note,1.4,'pluck')
        add(sig,at,gain,math.sin(step*.7)*.6)
        add(sig,at+.75*beat,gain*.25,-math.sin(step*.7)*.6)
    if genre not in ('Ambient','Piano'):
        for b in range(math.ceil(duration/beat)):
            at = b*beat
            t = np.arange(int(.22*SR),dtype=np.float32)/SR
            kick = np.sin(2*np.pi*(50*t+30*.018*(1-np.exp(-t/.018))))
            add(kick*np.exp(-t*20)*(1-np.exp(-t*650)),at,.18)
            if b%4 in (1,3):
                t=np.arange(int(.16*SR),dtype=np.float32)/SR
                snare=rng.normal(0,1,len(t)).astype(np.float32)*.38+np.sin(2*np.pi*180*t)*.4
                add(snare*np.exp(-t*30)*(1-np.exp(-t*900)),at,.1,.12)
            for off in (0,.5):
                t=np.arange(int(.07*SR),dtype=np.float32)/SR
                hat=np.diff(rng.normal(0,1,len(t)+1)).astype(np.float32)
                add(hat*np.exp(-t*80)*(1-np.exp(-t*1200)),at+off*beat,.009,.4 if off else -.4)
    lyric_events=[]
    if index in (0,1) and variation == 0:
        lines=json.loads((ROOT/'voices/lines.json').read_text(encoding='utf-8-sig'))
        filt=butter(2,[160,6100],btype='bandpass',fs=SR,output='sos')
        for j in range(16):
            number=index*8+j%8
            with wave.open(str(ROOT/f'voices/{number:02d}.wav'),'rb') as w:
                assert w.getframerate()==SR and w.getnchannels()==1
                speech=np.frombuffer(w.readframes(w.getnframes()),dtype='<i2').astype(np.float32)/32768
            speech=sosfilt(filt,speech).astype(np.float32)
            peak=np.max(np.abs(speech))
            speech*=.8/max(peak,1e-6)
            when=1.25+j*4.5 if j<8 else 42+(j-8)*4.5
            add(speech,when,.28)
            for delay,gain,pan in ((.16,.032,-.5),(.33,.024,.5),(.53,.016,-.2)):
                add(speech,when+delay,gain,pan)
            lyric_events.append((when,lines[number]))
    t=np.arange(count,dtype=np.float32)/SR
    audio=np.tanh(audio*1.2)
    audio*=.86/max(float(np.max(np.abs(audio))),1e-6)
    audio*=np.minimum(t/.04,1)[:,None]*np.minimum((duration-t)/1,1)[:,None]
    with wave.open(str(path),'wb') as w:
        w.setnchannels(2);w.setsampwidth(2);w.setframerate(SR)
        for n in range(0,count,SR):
            w.writeframes(np.int16(np.clip(audio[n:n+SR],-1,1)*32767).tobytes())
    return lyric_events

def main():
    LIB.mkdir(exist_ok=True)
    records=[]
    for i,(album,artist,genre,number,_,cover_name,titles) in enumerate(ALBUMS):
        folder=LIB/album
        folder.mkdir(exist_ok=True)
        if cover_name:
            shutil.copy2(ROOT/'covers'/cover_name,folder/'cover.png')
        elif album=='Quiet Rooms':
            oldmedia.artwork(8,album,(16,21,25),(185,202,201),folder/'cover.png')
        else:
            old_index=next(n for n,a in enumerate(oldmedia.ALBUMS) if a[0]==album)
            old_album=oldmedia.ALBUMS[old_index]
            oldmedia.artwork(old_index,album,old_album[2],old_album[3],folder/'cover.png')
        for j,title in enumerate(titles):
            if '--featured' in sys.argv and not(i<2 and j==0):
                continue
            duration=82 if i<2 and j==0 else 52+(i*7+j*11)%45
            lyrics=synth(i,j,duration,ROOT/'master.wav')
            target=folder/f'{j+1:02} - {title}.flac'
            subprocess.run([FFMPEG,'-hide_banner','-loglevel','error','-y','-i',str(ROOT/'master.wav'),
                '-i',str(folder/'cover.png'),'-map','0:a','-map','1:v',
                '-c:a','flac','-threads','1','-c:v','copy','-disposition:v','attached_pic',
                '-metadata',f'artist={artist}','-metadata',f'album_artist={artist}',
                '-metadata',f'album={album}','-metadata',f'title={title}',
                '-metadata',f'track={j+1}','-metadata',f'date={2023+i%4}',
                '-metadata',f'genre={genre}','-metadata',
                'comment=Original synthetic demonstration media; fictional catalogue for GODENplayer.',
                str(target)],check=True)
            if lyrics:
                lrc=[f'[ar:{artist}]',f'[al:{album}]',f'[ti:{title}]']
                lrc += [f'[{int(sec)//60:02}:{sec%60:05.2f}]{line}' for sec,line in lyrics]
                lrc += ['[01:19.00]']
                target.with_suffix('.lrc').write_text('\n'.join(lrc)+'\n',encoding='utf-8')
            records.append(dict(file=str(target.relative_to(ROOT)),artist=artist,album=album,
                title=title,genre=genre,duration=duration,sha256=sha(target),
                coverSha256=sha(folder/'cover.png'),timedLyrics=bool(lyrics)))
        print(f'{album}: {number} tracks ({genre})',flush=True)
    genres={g:sum(r['genre']==g for r in records) for g in sorted({r['genre'] for r in records})}
    if '--featured' in sys.argv:
        previous=json.loads((ROOT/'CATALOGUE.json').read_text())
        updated={r['file']:r for r in records}
        previous['tracks']=[updated.get(r['file'],r) for r in previous['tracks']]
        (ROOT/'CATALOGUE.json').write_text(json.dumps(previous,indent=2))
        return
    (ROOT/'CATALOGUE.json').write_text(json.dumps(dict(tracks=records,genreCounts=genres,
        originalAudio=True,originalLyrics=True,syntheticSpeech='Microsoft Zira Desktop',
        generatedCoverPrompts='covers/prompt-provenance.json'),indent=2))
    print('Total',len(records),'Genres',genres,flush=True)

if __name__=='__main__':main()
