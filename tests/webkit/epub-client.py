"""Standalone EPUB protocol regression inside the existing offline firmware container."""
from pathlib import Path
import base64
import hashlib
import json
import runpy
import shutil
import sys
import zipfile

shared = runpy.run_path(str(Path(__file__).with_name('webkit-client.py')), run_name='webkit_client')
Client, destination, bundle = (shared[key] for key in ('Client', 'destination', 'bundle'))


def encoded(value):
    return base64.b64encode(str(value).encode()).decode()


def fixture(path):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as book:
        book.writestr('mimetype', 'application/epub+zip')
        book.writestr('META-INF/container.xml', '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="EPUB/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        book.writestr('EPUB/book.opf', '<package xmlns="http://www.idpf.org/2007/opf" version="3.0"><manifest><item id="one" href="one.xhtml" media-type="application/xhtml+xml"/><item id="two" href="two.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="css" href="style.css" media-type="text/css"/><item id="font" href="font.ttf" media-type="font/ttf"/><item id="svg" href="picture.svg" media-type="image/svg+xml"/><item id="evil" href="evil.js" media-type="text/javascript"/></manifest><spine><itemref idref="one"/><itemref idref="two"/></spine></package>')
        book.writestr('EPUB/nav.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body><nav epub:type="toc"><ol><li><a href="one.xhtml">One</a></li><li><a href="two.xhtml#target">Two</a></li></ol></nav></body></html>')
        book.writestr('EPUB/style.css', '@import url("http://127.0.0.1:18765/css"); @font-face {font-family: Test; src: url(font.ttf)} body {font-family: Test} h1 {break-after:avoid} p {margin: 0 0 1em}')
        book.writestr('EPUB/font.ttf', (bundle / 'share/fixture/font.ttf').read_bytes())
        book.writestr('EPUB/picture.svg', '<svg xmlns="http://www.w3.org/2000/svg" width="180" height="80"><rect width="180" height="80" fill="navy"/><text x="10" y="40" fill="white">Local EPUB SVG</text></svg>')
        attack = "document.getElementById('guard').textContent='BOOK SCRIPT EXECUTED';fetch('http://127.0.0.1:18765/script')"
        book.writestr('EPUB/evil.js', attack)
        paragraphs = '<p id="unicode-target">Unique source anchor: Café 😀 e\u0301 العربية. A verified source position survives reflow.</p>'
        paragraphs += ''.join(f'<p>Paragraph {i:03}: A quiet reader turns the page. Typography and geometry stay tied to the source text through columns and viewport changes.</p>' for i in range(80))
        book.writestr('EPUB/one.xhtml', f'''<html xmlns="http://www.w3.org/1999/xhtml"><head><title>One</title><link rel="stylesheet" href="style.css"/><script>{attack}</script><script src="evil.js"></script><meta http-equiv="refresh" content="0;url=http://127.0.0.1:18765/refresh"/></head><body onload="{attack}"><h1 id="guard">Book scripts blocked</h1><a href="two.xhtml#target">Chapter two</a><img src="picture.svg"/><img src="http://127.0.0.1:18765/image"/><iframe src="http://127.0.0.1:18765/frame"></iframe>{paragraphs}</body></html>''')
        book.writestr('EPUB/two.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Two</title></head><body><h1 id="target">Second chapter target</h1><a href="one.xhtml">Return to chapter one</a><p>End of the focused fixture.</p></body></html>')


def break_fixture(path):
    fixture(path)
    with zipfile.ZipFile(path) as source:
        members = {name: source.read(name) for name in source.namelist()}
    members['EPUB/style.css'] = b'''@font-face {font-family: Test; src: url(font.ttf)}
body {font-family: Test} .box {height:80px} p {margin:0}
#before {page-break-before:always} #after {page-break-after:always}
#modern {break-before:page} #cascade {page-break-before:always;break-before:auto !important}
#media {page-break-before:always}
@media (min-width:700px) {#media {break-before:auto}}
#spacer {break-before:page;height:calc(100vh - 148px)}
#kept {page-break-inside:avoid} #kept div {height:120px}
'''
    members['EPUB/one.xhtml'] = b'''<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Breaks</title><link rel="stylesheet" href="style.css"/></head><body>
<section style="page-break-before:always;margin-top:30px"><div class="box" id="start" style="page-break-before:always">Start</div></section><div class="box" id="before">Legacy forced before</div>
<div class="box" id="after">Legacy forced after</div><div class="box" id="plain">After break</div>
<div class="box" id="modern">Modern forced page</div><div class="box" id="cascade">Cascade wins</div>
<div class="box" id="media">Media query</div><div id="spacer">Space before kept block</div>
<div id="kept"><div id="keepstart">Keep start</div><div id="keepend">Keep end</div></div>
<div class="box" id="left" style="page-break-before:left">Left forces a page</div>
<div class="box" id="right" style="page-break-before:right">Right forces a page</div>
<div style="break-before:page;height:calc(100vh - 148px)">Space before modern avoid</div>
<div style="break-inside:avoid-page"><div id="modernkeepstart" style="height:120px">Modern keep start</div><div id="modernkeepend" style="height:120px">Modern keep end</div></div>
<div style="break-before:page;height:calc(100vh - 148px)">Space before keep-with-previous</div>
<div class="box" id="beforeheading">Keep this heading</div><div class="box" id="beforebody" style="page-break-before:avoid">With this body</div>
<div style="break-before:page;height:calc(100vh - 148px)">Space before keep-with-next</div>
<div class="box" id="afterheading" style="page-break-after:avoid">Keep this heading</div><div class="box" id="afterbody">With this body</div>
</body></html>'''
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as book:
        for name, data in members.items():
            book.writestr(name, data)


def obfuscated_fixture(path, adobe):
    fixture(path)
    with zipfile.ZipFile(path) as source:
        members = {name: source.read(name) for name in source.namelist()}
    identifier = 'urn:uuid:00112233-4455-6677-8899-aabbccddeeff'
    metadata = '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">  ' + identifier + '\n </dc:identifier></metadata>'
    opf = members['EPUB/book.opf'].decode().replace('version="3.0"', 'version="3.0" unique-identifier="bookid"')
    members['EPUB/book.opf'] = opf.replace('<manifest>', metadata + '<manifest>').encode()
    key = bytes.fromhex('00112233445566778899aabbccddeeff') if adobe else hashlib.sha1(identifier.encode()).digest()
    font = bytearray(members['EPUB/font.ttf'])
    for index in range(min(len(font), 1024 if adobe else 1040)):
        font[index] ^= key[index % len(key)]
    members['EPUB/font.ttf'] = font
    algorithm = 'http://ns.adobe.com/pdf/enc#RC' if adobe else 'http://www.idpf.org/2008/embedding'
    members['META-INF/encryption.xml'] = ('<encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><EncryptedData xmlns="http://www.w3.org/2001/04/xmlenc#"><EncryptionMethod Algorithm="' + algorithm + '"/><CipherData><CipherReference URI="EPUB/font.ttf"/></CipherData></EncryptedData></encryption>').encode()
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as book:
        for name, data in members.items():
            book.writestr(name, data)


def image_fixture(path):
    fixture(path)
    with zipfile.ZipFile(path) as source:
        members = {name: source.read(name) for name in source.namelist()}
    opf = members['EPUB/book.opf'].decode()
    members['EPUB/book.opf'] = opf.replace('</manifest>', '<item id="image2" href="two.svg" media-type="image/svg+xml"/><item id="image3" href="three.svg" media-type="image/svg+xml"/></manifest>').encode()
    for name, color in (('two.svg', 'lime'), ('three.svg', 'maroon')):
        members['EPUB/' + name] = f'<svg xmlns="http://www.w3.org/2000/svg" width="200" height="300"><rect width="200" height="300" fill="{color}"/></svg>'.encode()
    members['EPUB/style.css'] = b'img {display:block;break-before:page;width:75%;height:70%;object-fit:contain}'
    members['EPUB/one.xhtml'] = b'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Image anchors</title><link rel="stylesheet" href="style.css"/></head><body><img src="picture.svg"/><img src="two.svg"/><img src="three.svg"/></body></html>'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as book:
        for name, data in members.items():
            book.writestr(name, data)


active = None
try:
    # The listener observes attempted loopback leaks; the container retains its
    # normal external-network restrictions. Content policy must stop these too.
    import http.server
    import threading
    hits = []
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            hits.append(self.path)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'blocked test endpoint')
        def log_message(self, *_args):
            pass
    server = http.server.HTTPServer(('127.0.0.1', 18765), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    synthetic = destination / 'pagination.epub'
    fixture(synthetic)
    active = Client('epub')
    assert active.receive() == active.ready
    request = 0
    frames = []
    transcript = []
    def call(action, error=None, release=True):
        global request
        request += 1
        identity = request
        active.send(f'KWRP/1 {identity} {action}\n')
        response = active.receive()
        assert response.startswith(f'KWRP/1 {identity} '), response
        if error:
            assert response == f'KWRP/1 {identity} ERROR {error}', response
            return None
        if ' OK PAGE ' not in response:
            assert ' OK ' in response, response
            return response
        metadata_path = Path(response.split()[-1])
        metadata = json.loads(metadata_path.read_text())
        active.runtime = metadata_path.parent
        pixels = Path(metadata['pixels']).read_bytes()
        assert metadata['frame'] == identity and metadata['format'] == 'BGRA8-premultiplied'
        assert len(pixels) == metadata['stride'] * metadata['height']
        assert metadata['stride'] >= metadata['width'] * 4
        metadata['sha256'] = hashlib.sha256(pixels).hexdigest()
        metadata['uniform_pixels'] = pixels == pixels[:4] * (len(pixels) // 4)
        transcript.append(metadata)
        shutil.copy2(metadata['png'], destination / f'page-{identity}.png')
        (destination / f'page-{identity}.json').write_text(json.dumps(metadata, indent=2))
        if release:
            call(f'RELEASE {identity}')
            assert not metadata_path.exists() and not Path(metadata['pixels']).exists()
        else:
            frames.append(identity)
        return metadata

    first = call(f'OPEN {encoded(synthetic)} 600 800')
    assert first['spine'] == 0 and first['layout']['pages'] > 1
    assert 'Book scripts blocked' in first['layout']['excerpt'], first
    assert len(first['document']['navigation']) == 2
    assert any(link['href'] == 'two.xhtml#target' for link in first['layout']['links'])
    second = call('NEXT')
    assert second['layout']['page'] == 1 and second['layout']['location']['start'] > first['layout']['location']['start']
    assert second['sha256'] != first['sha256']
    back = call('PREVIOUS')
    assert back['sha256'] == first['sha256'], (first, back)
    call('PREVIOUS', error='start-of-book')
    call('LINK ' + encoded('file:///etc/passwd'), error='invalid-link')
    call('LINK ' + encoded('javascript:alert(1)'), error='invalid-link')
    for missing in ('#missing', 'two.xhtml#missing'):
        call('LINK ' + encoded(missing), error='missing-link-target')
        restored = call('NEXT')
        assert restored['spine'] == 0 and restored['layout']['page'] == 1, restored
        call('PREVIOUS')
    linked = call('LINK ' + encoded('two.xhtml#target'))
    assert linked['spine'] == 1 and 'Second chapter target' in linked['layout']['excerpt']
    call('NEXT', error='end-of-book')
    previous_chapter = call('PREVIOUS')
    assert previous_chapter['spine'] == 0 and previous_chapter['layout']['page'] == previous_chapter['layout']['pages'] - 1
    next_chapter = call('NEXT')
    assert next_chapter['spine'] == 1 and next_chapter['layout']['page'] == 0
    middle = call('GOTO 0 3')
    resized = call('RESIZE 800 600')
    assert resized['width'] == 800 and resized['height'] == 600
    anchor = middle['layout']['location']['start']
    assert resized['layout']['location']['start'] <= anchor <= resized['layout']['location']['end'], (middle, resized)
    styled = call('STYLE 32 180 40 2 10 serif dark')
    assert styled['layout']['content']['x'] == 40 and styled['sha256'] != resized['sha256']
    call('STYLE 0 180 40 2 10 serif dark', error='invalid-style')
    call('GOTO 9 0', error='invalid-location')
    target = call('LINK ' + encoded('one.xhtml#unicode-target'))
    source = target['layout']['locator']
    assert source['version'] == 1 and source['after'].startswith('Unique source anchor:'), source
    assert source['cfi'].startswith('epubcfi(/4/2!'), source
    call('GOTO 1 0')
    restored = call('LOCATE 0 ' + encoded(json.dumps(source)))
    assert restored['layout']['locator'] == source
    call('RESIZE 600 900')
    restored = call('LOCATE 0 ' + encoded(json.dumps(source)))
    assert restored['layout']['locator'] == source
    call('CLOSE')
    call(f'OPEN {encoded(synthetic)} 600 800')
    restored = call('LOCATE 0 ' + encoded(json.dumps(source)))
    assert restored['layout']['locator'] == source, (source, restored)
    # Recover a parser-dependent structural path only through verified text.
    recover = dict(source, cfi='epubcfi(/4/2!/4/9999:0)', offset=source['offset'] + 7)
    restored = call('LOCATE 0 ' + encoded(json.dumps(recover)))
    assert restored['layout']['locator'] == source
    ambiguous = dict(source, cfi='epubcfi(/4/2!/4/9999:0)', offset=2147483647,
                     before='', after='Paragraph')
    call('LOCATE 0 ' + encoded(json.dumps(ambiguous)), error='unresolved-source-position')
    call('LOCATE 0 ' + encoded(json.dumps(dict(source, version=2))), error='invalid-source-position')
    call('LOCATE 0 ' + encoded('{bad JSON'), error='invalid-source-position')
    call('LOCATE 0 ' + encoded(json.dumps(source)))
    held = call('GOTO 0 0', release=False)
    call('NEXT', release=False)
    call('NEXT', error='frame-limit')
    assert Path(held['pixels']).is_file()
    for frame in frames:
        call(f'RELEASE {frame}')
    call('RELEASE 99999', error='invalid-frame')
    call('CLOSE')
    call('NEXT', error='no-book')
    call('STYLE 24 150 24 0 0 publisher light', error='no-book')
    breaks = destination / 'breaks.epub'
    break_fixture(breaks)
    call(f'OPEN {encoded(breaks)} 600 800')
    call('STYLE 24 150 24 0 0 publisher light')
    for fragment, expected in (('start', 0), ('before', 1), ('after', 1), ('plain', 2),
                               ('modern', 3), ('cascade', 3), ('media', 4),
                               ('keepstart', 6), ('keepend', 6), ('left', 7), ('right', 8),
                               ('modernkeepstart', 10), ('modernkeepend', 10),
                               ('beforeheading', 12), ('beforebody', 12),
                               ('afterheading', 14), ('afterbody', 14)):
        capture = call('LINK ' + encoded('#' + fragment))
        assert capture['layout']['page'] == expected, (fragment, expected, capture)
    call('RESIZE 800 600')
    capture = call('LINK ' + encoded('#media'))
    assert capture['layout']['page'] == 3, capture
    call('RESIZE 600 800')
    capture = call('LINK ' + encoded('#media'))
    assert capture['layout']['page'] == 4, capture
    call('STYLE 24 150 24 0 0 publisher light')
    for adobe in (False, True):
        obfuscated = destination / ('adobe.epub' if adobe else 'idpf.epub')
        obfuscated_fixture(obfuscated, adobe)
        capture = call(f'OPEN {encoded(obfuscated)} 600 800')
        assert capture['sha256'] == first['sha256'], (adobe, capture, first)
    folder = destination / ('long-' + 'x' * 180)
    folder.mkdir()
    long_book = folder / ('Unicode-\u03b2-' + 'name-' * 20 + '.epub')
    shutil.copy2(synthetic, long_book)
    capture = call(f'OPEN {encoded(long_book)} 600 800')
    assert capture['sha256'] == first['sha256']
    capture = call('LINK ' + encoded('./' * 150 + 'one.xhtml#guard'))
    assert capture['layout']['page'] == 0
    images = destination / 'images.epub'
    image_fixture(images)
    call(f'OPEN {encoded(images)} 600 800')
    image_page = call('NEXT')
    assert image_page['layout']['page'] == 1 and image_page['layout']['location']['element'] == [1], image_page
    resized_image = call('RESIZE 800 600')
    assert resized_image['layout']['page'] == 1 and resized_image['layout']['location']['element'] == [1], resized_image
    assert resized_image['layout']['objects'][0]['width'] > 0
    call('NEXT')
    image_back = call('PREVIOUS')
    assert image_back['layout']['page'] == 1
    # Open the checked-in general reading fixture, retaining the synthetic book
    # for focused assertions rather than using it as the representative book.
    representative = Path(__file__).with_name('trevelyans-type-tester.kepub.epub')
    real = call(f'OPEN {encoded(representative)} 800 1100')
    assert len(real['document']['spine']) > 10
    for index in (1, 2, 5, 10):
        page = call(f'GOTO {index} 0')
        assert page['spine'] == index and page['layout']['pages'] >= 1
        assert page['layout']['location']['end'] > page['layout']['location']['start']
    if len(sys.argv) > 4:
        corpus = Path(sys.argv[4])
        for book in sorted(corpus.glob('*.epub')):
            opened = call(f'OPEN {encoded(book)} 800 1100')
            call('STYLE 24 150 24 0 0 publisher light')
            for index in (2, 3, 5):
                page = call(f'GOTO {index} 0')
                assert page['layout']['location']['end'] > page['layout']['location']['start'], (book, index, page)
                assert not page['uniform_pixels'], (book, index, 'blank first page', page)
                if page['layout']['pages'] > 1:
                    forward = call('NEXT')
                    assert forward['layout']['page'] == 1
                    returned = call('PREVIOUS')
                    assert returned['sha256'] == page['sha256']
            navigation = opened['document']['navigation']
            if navigation:
                followed = call('LINK ' + encoded(navigation[-1]['href']))
                assert followed['layout']['page'] >= 0
    call('SHUTDOWN')
    active.close(0)
    active = None
    assert not hits, hits
    (destination / 'epub-results.json').write_text(json.dumps(dict(network_attempts=hits, pages=transcript, cleanup=True), indent=2))
    print('WEBKIT_EPUB_VERIFIED', flush=True)
finally:
    if active is not None:
        active.abort()
    if 'server' in globals():
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
