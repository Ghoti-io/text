"""Infer XML and Namespaces profiles from catalogue metadata and documents."""

import re


def profile_for(test, path):
    """Return the selected XML/Namespaces profile; absent version means 1.0."""
    version = test.get('VERSION')
    recommendation = test.get('RECOMMENDATION', '')
    if version is None:
        try:
            with open(path, 'rb') as fh:
                data = fh.read()
        except OSError:
            data = b''
        signatures = (
            (b'\xff\xfe\x00\x00', 'utf-32-le'),
            (b'\x00\x00\xfe\xff', 'utf-32-be'),
            (b'\xff\xfe', 'utf-16-le'),
            (b'\xfe\xff', 'utf-16-be'),
            (b'\xef\xbb\xbf', 'utf-8-sig'),
            (b'\x00\x00\x00<', 'utf-32-be'),
            (b'<\x00\x00\x00', 'utf-32-le'),
            (b'\x00<\x00?', 'utf-16-be'),
            (b'<\x00?\x00', 'utf-16-le'),
        )
        codec = next((name for signature, name in signatures if data.startswith(signature)), 'utf-8')
        sample = data.decode(codec, 'replace')
        if sample.startswith('\ufeff'):
            sample = sample[1:]
        declaration = re.match(r'^\s*<\?xml\s+[^?]*?version\s*=\s*(["\'])(.*?)\1', sample)
        version = declaration.group(2) if declaration else '1.0'
    if version == '1.1' or recommendation == 'XML1.1':
        xml_version = '1.1'
    else:
        xml_version = '1.0'
    if test.get('NAMESPACE') == 'no':
        namespace_version = 'none'
    elif recommendation == 'NS1.1' or (xml_version == '1.1' and recommendation != 'NS1.0'):
        namespace_version = '1.1'
    else:
        namespace_version = '1.0'
    return 'XML%s/NS%s' % (xml_version, namespace_version)
