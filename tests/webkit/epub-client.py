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
        paragraphs += '<p>rain<span>bow</span> co\u00adoperation extraordinary A𐐀word Reader’s 日本語 中文 Straße ﬁnal e‑mail Cafe\u0301 CAFÉ <span style="display:none">hiddenneedle</span></p><p>BlockAlpha</p><p>BlockBeta</p><p>LineAlpha<br/>LineBeta</p>'
        paragraphs += '<div id="selection" style="break-inside:avoid"><p>“Start finishes.” Another sentence!</p><p>Wait Dr. Smith waited…</p><p>Value 1.2. Next.</p><p>Alpha bravo charlie delta</p></div>'
        paragraphs += ''.join(f'<p>Paragraph {i:03}: A quiet reader turns the page. Typography and geometry stay tied to the source text through columns and viewport changes.</p>' for i in range(80))
        paragraphs += '<section id="selection-boundary" style="break-before:page"><p>Semantic selection boundary.</p></section>'
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

    def query(spine, view_page, fragment=None, **request):
        action = f'QUERY {spine} {view_page} ' + encoded(json.dumps(request))
        if fragment is not None:
            action += ' ' + encoded(fragment)
        response = call(action)
        envelope = json.loads(base64.b64decode(response.split(' OK JSON ', 1)[1]))
        assert envelope['spine'] == spine, envelope
        assert envelope['page'] == (view_page if fragment is None else envelope['data']['result']['page']), envelope
        assert 'error' not in envelope['data'], envelope
        return envelope['data']['result']

    first = call(f'OPEN {encoded(synthetic)} 600 800')
    assert first['spine'] == 0 and first['layout']['pages'] > 1
    assert 'Book scripts blocked' in first['layout']['excerpt'], first
    assert len(first['document']['navigation']) == 2
    assert any(link['href'] == 'two.xhtml#target' for link in first['layout']['links'])
    assert query(0, 0, kind='layout') == first['layout']
    summary = query(0, 0, kind='summary')
    assert summary['start'] == first['layout']['locator'] and summary['excerpt'], summary
    assert query(0, 0, kind='position', position=summary['start'])['page'] == 0
    query_checks = []
    def search(text, spine=0, pages=None):
        found = []
        for page in range(pages or first['layout']['pages']):
            part = query(spine, 0, kind='search', text=text, page=page)
            assert not part['limited'], part
            assert all(hit['page'] == page for hit in part['hits']), part
            found.extend(part['hits'])
        assert len({hit['start']['cfi'] for hit in found}) == len(found), found
        return found

    for text in ('rainbow', 'cooperation', 'extraordinary', 'A𐐀word', "Reader's", '日本語', '中文',
                 'STRASSE', 'final', 'e-mail', 'BlockAlpha BlockBeta', 'LineAlpha LineBeta'):
        found = search(text)
        assert len(found) == 1, (text, found)
        expected = {'cooperation': 'cooperation', "Reader's": 'Reader’s', 'STRASSE': 'Straße',
                    'final': 'ﬁnal', 'e-mail': 'e‑mail'}.get(text, text)
        assert found[0]['text'] == expected, (text, found[0])
        query_checks.append(text)
    assert len(search('CAFÉ')) == 3
    assert not search('hiddenneedle') and not search('BlockAlphaBlockBeta')
    assert len(search('page. Typography')) == 80
    for text in ('rainbow', 'A𐐀word', '日本語', 'العربية', 'Cafe\u0301'):
        hit = next(hit for hit in search(text) if hit['text'] == text)
        # Geometry and hit tests inspect the actual displayed column, including
        # words split across inline nodes and surrogate pairs.
        result = query(0, hit['page'], kind='range', start=hit['start'], end=hit['end'])
        assert result['text'] == text and result['rectangles'], result
        rect = result['rectangles'][0]
        word = query(0, hit['page'], kind='word', x=rect['x'] + rect['width'] / 2,
                     y=rect['y'] + rect['height'] / 2)
        assert word and word['text'] == text, (text, result, word)
        assert word['start']['offset'] == hit['start']['offset'] and word['end']['offset'] == hit['end']['offset']
        reverse = query(0, hit['page'], kind='range', start=hit['end'], end=hit['start'])
        assert reverse == result
        geometry = query(0, hit['page'], kind='ranges', ranges=[
            dict(start=hit['start'], end=hit['end']), dict(start=hit['end'], end=hit['start'])])['ranges']
        assert geometry[0] == geometry[1] and geometry[0]['rectangles'] == result['rectangles'], geometry
        edges = geometry[0]
        for name in ('startEdge', 'endEdge'):
            edge = edges[name]
            assert edge['visible'] and edge['first']['x'] == edge['last']['x'] and edge['first']['y'] < edge['last']['y'], edges
        direction = -1 if text == 'العربية' else 1
        assert edges['startEdge']['outward'] == dict(x=-direction, y=0), edges
        assert edges['endEdge']['outward'] == dict(x=direction, y=0), edges
        assert (edges['endEdge']['first']['x'] - edges['startEdge']['first']['x']) * direction > 0, edges
        resolved = query(0, 0, kind='resolve', positions=[hit['start'], hit['end'], dict(hit['start'], version=99)])['positions']
        assert resolved[:2] == [dict(locator=hit['start'], selectable=True), dict(locator=hit['end'], selectable=True)], resolved
        assert resolved[2] is None
    early, late = search('Paragraph 000')[0], search('Paragraph 079')[0]
    long_range = dict(start=early['start'], end=late['end'])
    for page, start_visible, end_visible in ((early['page'], True, False),
                                           (late['page'], False, True),
                                           ((early['page'] + late['page']) // 2, False, False)):
        geometry = query(0, page, kind='ranges', ranges=[long_range])['ranges'][0]
        assert geometry['rectangles'], geometry
        assert geometry['startEdge']['visible'] == start_visible and geometry['endEdge']['visible'] == end_visible, geometry
    selection_page = query(0, 0, kind='layout', fragment='selection')['page']
    def selection_hit(text):
        hit = query(0, 0, kind='search', text=text, page=selection_page)['hits'][0]
        return [hit['start'], hit['end']]

    def selection_range(first, last, start_policy=True, end_policy=True):
        return query(0, selection_page, kind='annotation-range', first=first, last=last,
                     startPolicy=start_policy, endPolicy=end_policy)

    def hit_rectangle(bounds):
        return query(0, selection_page, kind='range', start=bounds[0], end=bounds[1])['rectangles'][0]

    def hit_point(bounds):
        rect = hit_rectangle(bounds)
        return dict(x=rect['x'] + rect['width'] / 2, y=rect['y'] + rect['height'] / 2)

    start_word, end_word = selection_hit('Start'), selection_hit('finishes')
    complete = selection_range(start_word, end_word)
    assert complete['text'] == '“Start finishes.”', complete
    assert complete['chosenStart'] == start_word[0] and complete['chosenEnd'] == end_word[1], complete
    assert selection_range(end_word, start_word) == complete
    assert selection_range(end_word, end_word)['text'] == 'finishes'
    unquoted = selection_range(start_word, end_word, start_policy=False)
    assert unquoted['text'] == 'Start finishes.'
    assert selection_range(end_word, start_word, end_policy=False) == unquoted
    assert selection_range(start_word, selection_hit('sentence'))['text'] == '“Start finishes.” Another sentence!'
    assert selection_range([start_word[0]] * 2, [end_word[1]] * 2) == complete
    period_bounds = [end_word[1], selection_hit('finishes.')[1]]
    period = query(0, selection_page, kind='annotation-token', **hit_point(period_bounds))
    assert period and period['punctuation'] and period['text'] == '.', period
    exact = selection_range(start_word, [period['start'], period['end']], end_policy=False)
    assert exact['text'] == 'Start finishes.' and not exact['endPolicy'], exact
    retained = selection_range([start_word[0]] * 2, [period['end']] * 2,
                               start_policy=False, end_policy=False)
    assert retained['text'] == exact['text'] and retained['chosenEnd'] == period['end'], retained
    space = query(0, selection_page, kind='annotation-token', **hit_point([start_word[1], end_word[0]]))
    assert space and space['text'] == 'Start' and space['startBeforePunctuation'] == complete['start'], space
    assert selection_range(selection_hit('Wait'), selection_hit('Dr'))['text'] == 'Wait Dr'
    assert selection_range(selection_hit('Smith'), selection_hit('waited'))['text'] == 'Smith waited'
    assert selection_range(selection_hit('Value'), selection_hit('1.2'))['text'] == 'Value 1.2'
    alpha, bravo, charlie = map(selection_hit, ('Alpha', 'bravo', 'charlie'))
    assert query(0, selection_page, kind='drag-token', boundary=alpha[1],
                 start=False, **hit_point(bravo)) is None
    expanded = query(0, selection_page, kind='drag-token', boundary=alpha[1],
                     start=False, **hit_point(charlie))
    assert expanded and expanded['text'] == 'bravo', expanded
    expanded_back = query(0, selection_page, kind='drag-token', boundary=charlie[0],
                          start=True, **hit_point(alpha))
    assert expanded_back and expanded_back['text'] == 'bravo', expanded_back
    assert query(0, selection_page, kind='annotation-token', x=3, y=3) is None
    middle = (early['page'] + late['page']) // 2
    edges = query(0, middle, kind='page-edges')
    assert edges['first']['text'] and edges['last']['text'] and edges['before'] and edges['after'], edges
    semantic = query(0, 0, kind='layout', fragment='selection-boundary')['page']
    assert semantic > 0 and query(0, semantic, kind='page-edges')['before'] is None
    assert query(0, semantic - 1, kind='page-edges')['after'] is None
    assert query(1, 0, kind='page-edges')['before'] is None
    assert query(0, 0, kind='word', x=575, y=25) is None
    call('QUERY 0 0 ' + encoded('{bad JSON'), error='query-failed')
    call('QUERY 999 0 ' + encoded('{}'), error='invalid-query')
    # Queries don't allocate frames; inspect another spine while holding both
    # frame slots, then verify those copied pixels and the normal renderer.
    held_query = call('GOTO 0 0', release=False)
    other_held = call('NEXT', release=False)
    assert search('Second chapter target', 1, 1)
    inspected = query(0, 0, kind='layout', fragment='unicode-target')
    assert inspected['locator']['after'].startswith('Unique source anchor:'), inspected
    for spine in (0, 1):
        call(f'QUERY {spine} 0 ' + encoded(json.dumps({'kind': 'layout'})) + ' ' + encoded('missing'),
             error='missing-link-target')
        assert query(spine, 0, kind='layout')['pages'] >= 1
    assert Path(held_query['pixels']).is_file() and Path(other_held['pixels']).is_file()
    for frame in frames:
        call(f'RELEASE {frame}')
    frames.clear()
    after_queries = call('GOTO 0 0')
    assert after_queries['sha256'] == first['sha256']
    second = call('NEXT')
    assert second['layout']['page'] == 1 and second['layout']['location']['start'] > first['layout']['location']['start']
    assert second['sha256'] != first['sha256']
    summary_second = query(0, 1, kind='summary')
    located = query(0, 0, kind='position', position=summary_second['start'])
    assert located['page'] == 1, (summary_second, located)
    previous_end = query(0, 0, kind='position', position=summary_second['start'], exclusive=True)
    assert previous_end['page'] == 0, previous_end
    call('GOTO 0 1')
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
    page_summary = query(0, restored['layout']['page'], kind='summary')
    assert page_summary['start']['offset'] == restored['layout']['location']['start']
    assert query(0, 0, kind='position', position=source)['page'] == restored['layout']['page']
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
    image_summary = query(0, 1, kind='summary')
    assert image_summary['excerpt'] == '' and image_summary['start']['cfi'] == image_page['layout']['locator']['cfi']
    assert query(0, 0, kind='position', position=image_summary['start'])['page'] == 1
    call('GOTO 0 1')
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
    general_hits = []
    for index in range(len(real['document']['spine'])):
        chapter = call(f'GOTO {index} 0')
        for page in range(chapter['layout']['pages']):
            found = query(index, 0, kind='search', text='typography', page=page)
            assert not found['limited'], found
            for hit in found['hits']:
                assert hit['text'].lower() == 'typography', hit
            general_hits.extend(found['hits'])
    assert len(general_hits) == 3, general_hits
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
    (destination / 'epub-results.json').write_text(json.dumps(dict(network_attempts=hits, pages=transcript,
        text_queries=query_checks, source_range_geometry=True, word_hit_testing=True,
        annotation_terminal_edges=True, annotation_remote_edges=True, source_resolution=True,
        pointer_tokens=True, punctuation_provenance=True, handle_extension=True, page_edges=True,
        queries_preserve_frames=True, general_book_search=True,
        metadata_without_frames=True, cleanup=True), indent=2))
    print('WEBKIT_EPUB_VERIFIED', flush=True)
finally:
    if active is not None:
        active.abort()
    if 'server' in globals():
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
