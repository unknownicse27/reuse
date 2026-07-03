from setuptools import find_packages
from setuptools import setup

from reuse import __version__


setup(
    name='reuse',
    version=__version__,
    description='reuse: Maximizing the Power of Symbolic Execution by Adaptively Tuning External Parameters',
    python_version='>=3.9',
    packages=find_packages(include=('reuse', 'reuse.*')),
    include_package_data=True,
    setup_requires=[],
    install_requires=[],
    dependency_links=[],
    entry_points={
        'console_scripts': [
            'reuse=reuse.bin:main',
        ]
    }
)
