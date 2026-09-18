#include "builtin_registry.h"
#include "builtin_registry_tables.h"

#include <unordered_map>
#include <unordered_set>

namespace ir
{

// ── Python builtins (guaranteed by CPython) ─────────────────────
const std::unordered_set<std::string> &pyBuiltins()
{
	static const std::unordered_set<std::string> *s =
		new std::unordered_set<std::string>{
			// Built-in functions — always available
			"abs",
			"all",
			"any",
			"ascii",
			"bin",
			"bool",
			"bytearray",
			"bytes",
			"callable",
			"chr",
			"classmethod",
			"compile",
			"complex",
			"delattr",
			"dict",
			"dir",
			"divmod",
			"enumerate",
			"eval",
			"exec",
			"filter",
			"float",
			"format",
			"frozenset",
			"getattr",
			"globals",
			"hasattr",
			"hash",
			"hex",
			"id",
			"input",
			"int",
			"isinstance",
			"issubclass",
			"iter",
			"len",
			"list",
			"locals",
			"map",
			"max",
			"memoryview",
			"min",
			"next",
			"object",
			"oct",
			"open",
			"ord",
			"pow",
			"print",
			"property",
			"range",
			"repr",
			"reversed",
			"round",
			"set",
			"setattr",
			"slice",
			"sorted",
			"staticmethod",
			"str",
			"sum",
			"super",
			"tuple",
			"type",
			"vars",
			"zip",
			"__import__",
			// Built-in exceptions (commonly raised/caught by name)
			"Exception",
			"ValueError",
			"TypeError",
			"KeyError",
			"IndexError",
			"AttributeError",
			"RuntimeError",
			"StopIteration",
			"NotImplementedError",
			"AssertionError",
			"ImportError",
			"ModuleNotFoundError",
			"FileNotFoundError",
			"OSError",
			"IOError",
			"ZeroDivisionError",
			"NameError",
			"UnboundLocalError",
			"SyntaxError",
			"IndentationError",
			"TabError",
			"SystemExit",
			"KeyboardInterrupt",
			"GeneratorExit",
			"BaseException",
			// Common built-in types/classes
			"True",
			"False",
			"None",
			"Ellipsis",
			"NotImplemented",
		};
	return *s;
}

const std::unordered_set<std::string> &pyStdlib()
{
	static const std::unordered_set<
		std::string> *s = new std::unordered_set<std::string>{
		// Python standard library — most commonly used modules and their
		// top-level functions. These are project-external but well-known.
		// Loaded as: `import os` → os.path.join(...) references "join"
		// We only register the "short name" (method/function) here.
		// Full qualified names are resolved via the import map.
		"os",
		"sys",
		"re",
		"json",
		"math",
		"time",
		"datetime",
		"collections",
		"itertools",
		"functools",
		"pathlib",
		"shutil",
		"subprocess",
		"typing",
		"enum",
		"dataclasses",
		"abc",
		"copy",
		"random",
		"statistics",
		"hashlib",
		"base64",
		"uuid",
		"tempfile",
		"logging",
		"warnings",
		"traceback",
		"pickle",
		"io",
		"threading",
		"multiprocessing",
		"asyncio",
		"concurrent",
		"unittest",
		"pytest",
		"doctest",
		"csv",
		"configparser",
		"xml",
		"html",
		"textwrap",
		"argparse",
		"getopt",
		"platform",
		"socket",
		"http",
		"email",
		"urllib",
		"requests",
		"httpx",
	};
	return *s;
}

const std::unordered_set<std::string> &pyThirdParty()
{
	static const std::unordered_set<std::string> *s =
		new std::unordered_set<std::string>{
			// High-frequency third-party library symbols
			// numpy
			"numpy",
			"np",
			"ndarray",
			"array",
			"zeros",
			"ones",
			"empty",
			"arange",
			"linspace",
			"reshape",
			"transpose",
			"concatenate",
			"stack",
			"split",
			"dot",
			"matmul",
			"sum",
			"mean",
			"std",
			"var",
			"min",
			"max",
			"argmin",
			"argmax",
			"sort",
			"unique",
			"where",
			"clip",
			"abs",
			"sqrt",
			"exp",
			"log",
			"sin",
			"cos",
			"linspace",
			"meshgrid",
			"random",
			"seed",
			"normal",
			"uniform",
			"randint",
			"randn",
			"rand",
			"histogram",
			"fft",
			"ifft",
			// pandas
			"pandas",
			"pd",
			"DataFrame",
			"Series",
			"read_csv",
			"read_excel",
			"read_json",
			"read_sql",
			"to_csv",
			"to_excel",
			"to_json",
			"to_sql",
			"merge",
			"join",
			"concat",
			"groupby",
			"pivot_table",
			"melt",
			"crosstab",
			"cut",
			"qcut",
			"get_dummies",
			"dropna",
			"fillna",
			"replace",
			"astype",
			"value_counts",
			"sort_values",
			"sort_index",
			"reset_index",
			"set_index",
			"loc",
			"iloc",
			"at",
			"iat",
			"apply",
			"applymap",
			"pipe",
			// matplotlib
			"matplotlib",
			"plt",
			"figure",
			"plot",
			"scatter",
			"bar",
			"hist",
			"pie",
			"imshow",
			"contour",
			"subplot",
			"subplots",
			"xlabel",
			"ylabel",
			"title",
			"legend",
			"colorbar",
			"clf",
			"cla",
			"close",
			"savefig",
			"show",
			"tight_layout",
			"grid",
			"axis",
			"xlim",
			"ylim",
			"xticks",
			"yticks",
			"text",
			"annotate",
			"arrow",
			"fill_between",
			"stackplot",
			"semilogx",
			"semilogy",
			// plotly
			"plotly",
			"go",
			"px",
			"Figure",
			"Scatter",
			"Bar",
			"Pie",
			"Histogram",
			"Box",
			"Violin",
			"Heatmap",
			"Contour",
			"Surface",
			"Mesh3d",
			"Scatter3d",
			"Scattergeo",
			"add_trace",
			"add_scatter",
			"add_bar",
			"add_histogram",
			"add_vline",
			"add_hline",
			"add_vrect",
			"add_hrect",
			"add_shape",
			"add_annotation",
			"add_layout_image",
			"update_layout",
			"update_traces",
			"update_xaxes",
			"update_yaxes",
			"show",
			"write_html",
			"write_image",
			"to_html",
			"to_json",
			"from_json",
			"make_subplots",
			// torch
			"torch",
			"tensor",
			"FloatTensor",
			"LongTensor",
			"zeros",
			"ones",
			"rand",
			"randn",
			"arange",
			"cat",
			"stack",
			"split",
			"chunk",
			"gather",
			"scatter",
			"index_select",
			"masked_select",
			"nonzero",
			"where",
			"clone",
			"detach",
			"requires_grad_",
			"backward",
			"grad",
			"no_grad",
			"enable_grad",
			"set_grad_enabled",
			"nn",
			"Module",
			"Linear",
			"Conv2d",
			"LSTM",
			"GRU",
			"Embedding",
			"Dropout",
			"BatchNorm1d",
			"BatchNorm2d",
			"LayerNorm",
			"ReLU",
			"Sigmoid",
			"Tanh",
			"Softmax",
			"LogSoftmax",
			"CrossEntropyLoss",
			"MSELoss",
			"L1Loss",
			"NLLLoss",
			"optim",
			"SGD",
			"Adam",
			"AdamW",
			"RMSprop",
			"lr_scheduler",
			"DataLoader",
			"Dataset",
			"TensorDataset",
			"random_split",
			"save",
			"load",
			"jit",
			"onnx",
			"utils",
			"functional",
			// tensorflow / keras
			"tensorflow",
			"tf",
			"keras",
			"Sequential",
			"Model",
			"Layer",
			"Dense",
			"Conv2D",
			"MaxPooling2D",
			"Flatten",
			"Dropout",
			"LSTM",
			"GRU",
			"Embedding",
			"BatchNormalization",
			"compile",
			"fit",
			"predict",
			"evaluate",
			"save",
			"load_model",
			// scikit-learn
			"sklearn",
			"train_test_split",
			"cross_val_score",
			"GridSearchCV",
			"RandomizedSearchCV",
			"Pipeline",
			"make_pipeline",
			"LinearRegression",
			"LogisticRegression",
			"SVM",
			"SVC",
			"RandomForestClassifier",
			"RandomForestRegressor",
			"GradientBoostingClassifier",
			"KMeans",
			"PCA",
			"TSNE",
			"StandardScaler",
			"MinMaxScaler",
			"LabelEncoder",
			"accuracy_score",
			"precision_score",
			"recall_score",
			"f1_score",
			"confusion_matrix",
			"classification_report",
			// FastAPI / Flask
			"FastAPI",
			"flask",
			"Flask",
			"app",
			"Route",
			"get",
			"post",
			"put",
			"delete",
			"patch",
			"Request",
			"Response",
			"JSONResponse",
			"Body",
			"Query",
			"Path",
			"Header",
			"Cookie",
			"Form",
			"File",
			"UploadFile",
			"Depends",
			"HTTPException",
			"APIRouter",
			// Pydantic
			"pydantic",
			"BaseModel",
			"Field",
			"validator",
			"root_validator",
			// SQLAlchemy
			"sqlalchemy",
			"Column",
			"Integer",
			"String",
			"Float",
			"Boolean",
			"DateTime",
			"ForeignKey",
			"Table",
			"create_engine",
			"sessionmaker",
			"scoped_session",
			"declarative_base",
			"relationship",
			"backref",
			// pytest
			"pytest",
			"fixture",
			"mark",
			"parametrize",
			"skip",
			"skipif",
			"raises",
			"approx",
			"config",
			"tmpdir",
			"tmp_path",
			"monkeypatch",
			// Other common
			"tqdm",
			"tqdm_notebook",
			"click",
			"rich",
			"progress",
			"loguru",
			"structlog",
			"sentry_sdk",
			"dataclass",
			"field",
			"asdict",
			"astuple",
			"decorator",
			"wraps",
			"lru_cache",
			"cached_property",
			"partial",
			"reduce",
			"singledispatch",
		};
	return *s;
}

// ── Registry dispatch ───────────────────────────────────────────
// Map language → symbol set
using SymbolTable = std::unordered_set<std::string>;
using RegistryMap =
	std::unordered_map<std::string, std::vector<const SymbolTable *>>;

static const RegistryMap &registry()
{
	static const RegistryMap *m = new RegistryMap{
		{ "python", { &pyBuiltins(), &pyStdlib(), &pyThirdParty() } },
		{ "c", { &cBuiltins() } },
		{ "cpp", { &cBuiltins(), &cppBuiltins() } },
		{ "rust", { &rustBuiltins() } },
		{ "go", { &goBuiltins() } },
		{ "java", { &javaBuiltins() } },
		{ "javascript", { &jsBuiltins() } },
		{ "typescript", { &jsBuiltins() } },
		{ "js", { &jsBuiltins() } },
		{ "ts", { &jsBuiltins() } },
		{ "swift", { &swiftBuiltins() } },
	};
	return *m;
}

std::string BuiltinRegistry::resolve(const std::string &language,
				     const std::string &name)
{
	if (name.empty())
		return "unresolved";

	auto it = registry().find(language);
	if (it == registry().end()) {
		// Unknown language — fall back to checking all tables
		for (const auto &[lang, tables] : registry()) {
			(void)lang;
			for (const auto *table : tables) {
				if (table->count(name))
					return "external";
			}
		}
		return "unresolved";
	}

	for (const auto *table : it->second) {
		if (table->count(name))
			return "external";
	}

	return "unresolved";
}

bool BuiltinRegistry::isKnownExternal(const std::string &name)
{
	for (const auto &[lang, tables] : registry()) {
		(void)lang;
		for (const auto *table : tables) {
			if (table->count(name))
				return true;
		}
	}
	return false;
}

const std::unordered_set<std::string> &
BuiltinRegistry::externalSymbols(const std::string &language)
{
	static const std::unordered_set<std::string> empty;
	auto it = registry().find(language);
	if (it == registry().end())
		return empty;
	// Return the union of all tables for this language.
	// We use a static cache to avoid recomputing on each call.
	static std::unordered_map<std::string, std::unordered_set<std::string>>
		cache;
	auto cache_it = cache.find(language);
	if (cache_it != cache.end())
		return cache_it->second;
	std::unordered_set<std::string> merged;
	for (const auto *table : it->second) {
		for (const auto &sym : *table)
			merged.insert(sym);
	}
	auto result = cache.emplace(language, std::move(merged));
	return result.first->second;
}

} // namespace ir
