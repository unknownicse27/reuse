from setuptools import find_packages
from setuptools import setup
from reuse import __version__

setup(
    name='reuse',
    version=__version__,
    description='ReuSE: Refining Query Results for Efficient Symbolic Execution Across Software Releases',
    python_requires='>=3.9',
    packages=find_packages(include=('reuse', 'reuse.*')),
    include_package_data=True,
    setup_requires=[],
    install_requires=[
        'zstandard>=0.21.0',
        'z3-solver>=4.12.0',
    ],
    dependency_links=[],
    entry_points={
        'console_scripts': [
            'reuse=reuse.bin:main',
        ]
    }
)
